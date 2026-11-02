#include "charger_test.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>

#include "board_config.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_io_expander.h"
#include "esp_io_expander_xl9555.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

namespace {

constexpr char kTag[] = "charger_hw";
constexpr int kI2cTimeoutMs = 100;
constexpr int kRetryDelayMs = 1000;
constexpr int kReadFailureLimit = 3;
constexpr uint8_t kExpectedPartId = 0x03;

constexpr uint8_t kRegVreg = 0x00;
constexpr uint8_t kRegIchg = 0x01;
constexpr uint8_t kRegIindpm = 0x03;
constexpr uint8_t kRegPrechargeTermination = 0x04;
constexpr uint8_t kRegChargerControl1 = 0x05;
constexpr uint8_t kRegChargerControl2 = 0x06;
constexpr uint8_t kRegChargerControl3 = 0x07;
constexpr uint8_t kRegStatus1 = 0x0B;
constexpr uint8_t kRegStatus2 = 0x0C;
constexpr uint8_t kRegNtcStatus = 0x0D;
constexpr uint8_t kRegFaultStatus = 0x0E;
constexpr uint8_t kRegAdcControl = 0x15;
constexpr uint8_t kRegAdcDisable = 0x16;
constexpr uint8_t kRegPartInformation = 0x25;

constexpr uint8_t kEnHizMask = 1U << 7;
constexpr uint8_t kEnIlimMask = 1U << 6;
constexpr uint8_t kIchgMask = 0x3F;
constexpr uint8_t kIindpmMask = 0x1F;
constexpr uint8_t kWatchdogMask = 0x30;
constexpr uint8_t kEnOtgMask = 1U << 7;
constexpr uint8_t kThermalRegulationMask = 0x30;
constexpr uint8_t kEnChargeMask = 1U << 3;
constexpr uint8_t kWatchdogResetMask = 1U << 6;
constexpr uint8_t kAdcEnabledContinuous12Bit = 0xB0;
constexpr int kLikelySingleCellMinMv = 2500;
constexpr int kLikelySingleCellMaxMv = 4500;

struct ChargerRawSnapshot {
    std::array<uint8_t, 0x0F> control = {};
    std::array<uint8_t, 0x11> adc = {};
    uint32_t io_levels = 0;
};

static_assert(
    BOARD_CHARGER_VREG_MV >= 6800 && BOARD_CHARGER_VREG_MV <= 9200 &&
        BOARD_CHARGER_VREG_MV % 10 == 0,
    "VREG must be 6800..9200mV in 10mV steps");
static_assert(
    BOARD_CHARGER_ICHG_MA >= 100 && BOARD_CHARGER_ICHG_MA <= 2200 &&
        BOARD_CHARGER_ICHG_MA % 50 == 0,
    "ICHG must be 100..2200mA in 50mA steps");
static_assert(
    BOARD_CHARGER_IINDPM_MA >= 500 && BOARD_CHARGER_IINDPM_MA <= 3300 &&
        (BOARD_CHARGER_IINDPM_MA - 500) % 100 == 0,
    "IINDPM must be 500..3300mA in 100mA steps");
static_assert(
    BOARD_CHARGER_PRECHARGE_MA >= 50 &&
        BOARD_CHARGER_PRECHARGE_MA <= 800 &&
        (BOARD_CHARGER_PRECHARGE_MA - 50) % 50 == 0,
    "IPRECHG must be 50..800mA in 50mA steps");
static_assert(
    BOARD_CHARGER_TERMINATION_MA >= 50 &&
        BOARD_CHARGER_TERMINATION_MA <= 800 &&
        (BOARD_CHARGER_TERMINATION_MA - 50) % 50 == 0,
    "ITERM must be 50..800mA in 50mA steps");
static_assert(
    BOARD_CHARGER_TREG_C == 60 || BOARD_CHARGER_TREG_C == 80 ||
        BOARD_CHARGER_TREG_C == 100 || BOARD_CHARGER_TREG_C == 120,
    "TREG must be 60, 80, 100, or 120 degrees C");
static_assert(
    BOARD_CHARGER_MIN_SAFE_2S_MV >= 4400 &&
        BOARD_CHARGER_MIN_SAFE_2S_MV < BOARD_CHARGER_VREG_MV,
    "minimum safe 2S voltage must be at least 4400mV and below VREG");
static_assert(
    BOARD_CHARGER_MAX_SAFE_2S_MV >= BOARD_CHARGER_VREG_MV &&
        BOARD_CHARGER_MAX_SAFE_2S_MV <= 9200,
    "maximum safe 2S voltage must include VREG and not exceed 9200mV");
static_assert(
    (CONFIG_XL9555_DEFAULT_OUTPUT & BOARD_XL9555_P02_CHARGE_EN) != 0,
    "XL9555 P02 output latch must default high to keep charger nCE disabled");

i2c_master_bus_handle_t s_i2c_bus = nullptr;
i2c_master_dev_handle_t s_charger = nullptr;
esp_io_expander_handle_t s_io_expander = nullptr;
ChargerStateCallback s_callback = nullptr;
void *s_callback_context = nullptr;
ChargerTestState s_state = {};
ChargerRawSnapshot s_raw_snapshot = {};
bool s_task_started = false;

esp_err_t read_snapshot();

void publish_state()
{
    if (s_callback != nullptr) {
        s_callback(s_state, s_callback_context);
    }
}

esp_err_t read_registers(uint8_t first_register, uint8_t *data, size_t length)
{
    return i2c_master_transmit_receive(
        s_charger,
        &first_register,
        1,
        data,
        length,
        kI2cTimeoutMs);
}

esp_err_t read_register(uint8_t register_address, uint8_t *value)
{
    return read_registers(register_address, value, 1);
}

esp_err_t write_register(uint8_t register_address, uint8_t value)
{
    const uint8_t data[] = {register_address, value};
    return i2c_master_transmit(
        s_charger,
        data,
        sizeof(data),
        kI2cTimeoutMs);
}

esp_err_t update_register(uint8_t register_address, uint8_t mask, uint8_t value)
{
    uint8_t current = 0;
    ESP_RETURN_ON_ERROR(
        read_register(register_address, &current),
        kTag,
        "read register 0x%02X",
        register_address);
    current = static_cast<uint8_t>((current & ~mask) | (value & mask));
    return write_register(register_address, current);
}

uint8_t encode_vreg(int millivolts)
{
    const int clamped = std::clamp(millivolts, 6800, 9200);
    return static_cast<uint8_t>((clamped - 6800) / 10);
}

uint8_t encode_ichg(int milliamps)
{
    const int clamped = std::clamp(milliamps, 100, 2200);
    return static_cast<uint8_t>(clamped / 50);
}

uint8_t encode_iindpm(int milliamps)
{
    const int clamped = std::clamp(milliamps, 500, 3300);
    return static_cast<uint8_t>((clamped - 500) / 100);
}

uint8_t encode_precharge_or_termination(int milliamps)
{
    const int clamped = std::clamp(milliamps, 50, 800);
    return static_cast<uint8_t>((clamped - 50) / 50);
}

uint8_t encode_thermal_regulation(int celsius)
{
    return static_cast<uint8_t>(((celsius - 60) / 20) << 4);
}

int decode_vreg(uint8_t value)
{
    return 6800 + std::min<int>(value, 240) * 10;
}

int decode_ichg(uint8_t value)
{
    return std::clamp<int>((value & 0x3F) * 50, 100, 2200);
}

int decode_iindpm(uint8_t value)
{
    return 500 + std::min<int>(value & 0x1F, 28) * 100;
}

int decode_precharge_or_termination(uint8_t value)
{
    return 50 + (value & 0x0F) * 50;
}

int decode_thermal_regulation(uint8_t value)
{
    return 60 + ((value >> 4) & 0x03) * 20;
}

int decode_adc_unsigned(uint8_t high, uint8_t low, uint8_t high_mask)
{
    return ((high & high_mask) << 8) | low;
}

int decode_ibus(uint8_t high, uint8_t low)
{
    const int magnitude = decode_adc_unsigned(high, low, 0x0F);
    return (high & 0x80) != 0 ? -magnitude : magnitude;
}

bool profile_matches(const ChargerRawSnapshot &raw)
{
    const auto &reg = raw.control;
    const uint8_t expected_control2 = static_cast<uint8_t>(
        encode_thermal_regulation(BOARD_CHARGER_TREG_C));
    return reg[kRegVreg] == encode_vreg(BOARD_CHARGER_VREG_MV) &&
           (reg[kRegIchg] & (kEnHizMask | kIchgMask)) ==
               encode_ichg(BOARD_CHARGER_ICHG_MA) &&
           (reg[kRegIindpm] & kIindpmMask) <=
               encode_iindpm(BOARD_CHARGER_IINDPM_MA) &&
           reg[kRegPrechargeTermination] == static_cast<uint8_t>(
               (encode_precharge_or_termination(BOARD_CHARGER_PRECHARGE_MA) << 4) |
               encode_precharge_or_termination(BOARD_CHARGER_TERMINATION_MA)) &&
           (reg[kRegChargerControl1] & kWatchdogMask) == 0 &&
           (reg[kRegChargerControl2] &
            (kEnOtgMask | kThermalRegulationMask)) == expected_control2 &&
           (reg[kRegChargerControl3] & kWatchdogResetMask) == 0 &&
           (raw.adc[0] & 0xF0) == kAdcEnabledContinuous12Bit &&
           ((raw.adc[0x10] >> 3) & 0x0F) == kExpectedPartId;
}

void log_register_snapshot(const char *reason)
{
    const auto &reg = s_raw_snapshot.control;
    const auto &adc = s_raw_snapshot.adc;
    ESP_LOGI(
        kTag,
        "%s raw: 00=%02X 01=%02X 03=%02X 04=%02X 05=%02X 06=%02X "
        "07=%02X 0B=%02X 0C=%02X 0D=%02X 0E=%02X 15=%02X 25=%02X XLIN=%04X",
        reason,
        reg[kRegVreg],
        reg[kRegIchg],
        reg[kRegIindpm],
        reg[kRegPrechargeTermination],
        reg[kRegChargerControl1],
        reg[kRegChargerControl2],
        reg[kRegChargerControl3],
        reg[kRegStatus1],
        reg[kRegStatus2],
        reg[kRegNtcStatus],
        reg[kRegFaultStatus],
        adc[0],
        adc[0x10],
        static_cast<unsigned>(s_raw_snapshot.io_levels & 0xFFFF));
    ESP_LOGI(
        kTag,
        "%s gates: nCE=%s EN_CHG=%u EN_HIZ=%u NTC=%u VBUS_SRC=%u "
        "IINDPM=%u VINDPM=%u TREG=%u PG=%u",
        reason,
        (s_raw_snapshot.io_levels & BOARD_XL9555_P02_CHARGE_EN) != 0 ?
            "HIGH(disabled)" : "LOW(enabled)",
        (reg[kRegChargerControl2] & kEnChargeMask) != 0,
        (reg[kRegIchg] & kEnHizMask) != 0,
        reg[kRegNtcStatus] & 0x07,
        (reg[kRegStatus2] >> 4) & 0x07,
        (reg[kRegStatus1] & 0x40) != 0,
        (reg[kRegStatus1] & 0x20) != 0,
        (reg[kRegStatus1] & 0x10) != 0,
        (reg[kRegStatus2] & 0x80) != 0);
}

esp_err_t set_charge_pin_enabled(bool enabled)
{
    // SGM41529 nCE is active-low. Keep the pin high until configuration and
    // part-ID checks have completed.
    return esp_io_expander_set_level(
        s_io_expander,
        BOARD_XL9555_P02_CHARGE_EN,
        enabled ? 0 : 1);
}

void disable_charge_best_effort()
{
    if (s_io_expander != nullptr) {
        const esp_err_t err = set_charge_pin_enabled(false);
        if (err != ESP_OK) {
            ESP_LOGE(kTag, "Unable to deassert nCE: %s", esp_err_to_name(err));
            s_state.configuration_valid = false;
        } else {
            s_state.charge_pin_enabled = false;
        }
    }
    if (s_charger != nullptr) {
        const esp_err_t err = update_register(
            kRegChargerControl2,
            kEnChargeMask,
            0);
        if (err != ESP_OK) {
            ESP_LOGE(kTag, "Unable to clear EN_CHG: %s", esp_err_to_name(err));
            s_state.configuration_valid = false;
        } else {
            s_state.register_charge_enabled = false;
        }
    }
    s_state.charge_enabled = false;
}

esp_err_t init_i2c_and_expander()
{
    i2c_master_bus_config_t bus_config = {};
    bus_config.i2c_port = BOARD_I2C_PORT;
    bus_config.sda_io_num = BOARD_I2C_SDA;
    bus_config.scl_io_num = BOARD_I2C_SCL;
    bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_config.glitch_ignore_cnt = 7;
    bus_config.flags.enable_internal_pullup = true;
    ESP_RETURN_ON_ERROR(
        i2c_new_master_bus(&bus_config, &s_i2c_bus),
        kTag,
        "create I2C bus");

    ESP_RETURN_ON_ERROR(
        esp_io_expander_new_i2c_xl9555(
            s_i2c_bus,
            BOARD_XL9555_I2C_ADDR,
            &s_io_expander),
        kTag,
        "create XL9555");

    ESP_RETURN_ON_ERROR(
        esp_io_expander_set_dir(
            s_io_expander,
            BOARD_XL9555_P02_CHARGE_EN,
            IO_EXPANDER_OUTPUT),
        kTag,
        "configure charge enable output");
    ESP_RETURN_ON_ERROR(
        esp_io_expander_set_level(
            s_io_expander,
            BOARD_XL9555_P02_CHARGE_EN,
            1),
        kTag,
        "disable charge before configuration");
    ESP_RETURN_ON_ERROR(
        esp_io_expander_set_dir(
            s_io_expander,
            BOARD_XL9555_P03_CHARGE_INT |
                BOARD_XL9555_P04_CHARGE_GOOD,
            IO_EXPANDER_INPUT),
        kTag,
        "configure charger status inputs");

    i2c_device_config_t charger_config = {};
    charger_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    charger_config.device_address = BOARD_CHARGER_I2C_ADDR;
    charger_config.scl_speed_hz = BOARD_CHARGER_I2C_FREQ_HZ;
    ESP_RETURN_ON_ERROR(
        i2c_master_bus_add_device(
            s_i2c_bus,
            &charger_config,
            &s_charger),
        kTag,
        "add SGM41529 I2C device");

    ESP_LOGI(
        kTag,
        "I2C ready: SDA=%d SCL=%d, charger=0x%02X, XL9555=0x%02X",
        BOARD_I2C_SDA,
        BOARD_I2C_SCL,
        BOARD_CHARGER_I2C_ADDR,
        BOARD_XL9555_I2C_ADDR);
    return ESP_OK;
}

esp_err_t configure_charger()
{
    disable_charge_best_effort();
    s_state.configuration_valid = false;
    s_state.charge_enabled = false;
    s_state.adc_enabled = false;
    s_state.watchdog_expired = false;

    uint8_t part_information = 0;
    const esp_err_t part_err =
        read_register(kRegPartInformation, &part_information);
    if (part_err != ESP_OK) {
        s_state.device_present = false;
        s_state.part_id_valid = false;
        std::snprintf(
            s_state.detail,
            sizeof(s_state.detail),
            "No SGM41529 response; connect battery or VBUS");
        return part_err;
    }

    s_state.device_present = true;
    s_state.part_id = (part_information >> 3) & 0x0F;
    s_state.revision = part_information & 0x07;
    s_state.part_id_valid = s_state.part_id == kExpectedPartId;
    if (!s_state.part_id_valid) {
        std::snprintf(
            s_state.detail,
            sizeof(s_state.detail),
            "Unexpected charger ID: PN=%u REV=%u",
            s_state.part_id,
            s_state.revision);
        return ESP_ERR_NOT_FOUND;
    }

    // Disable OTG and charging before changing the profile. The lower thermal
    // regulation threshold is intentional for unattended bench observation.
    ESP_RETURN_ON_ERROR(
        update_register(
            kRegChargerControl2,
            kEnOtgMask | kThermalRegulationMask | kEnChargeMask,
            encode_thermal_regulation(BOARD_CHARGER_TREG_C)),
        kTag,
        "disable charge path and set thermal regulation");

    // Enter host mode and then disable the watchdog so the programmed profile
    // is not reverted to the autonomous defaults after 40 seconds.
    ESP_RETURN_ON_ERROR(
        update_register(
            kRegChargerControl3,
            kWatchdogResetMask,
            kWatchdogResetMask),
        kTag,
        "enter charger host mode");
    ESP_RETURN_ON_ERROR(
        update_register(kRegChargerControl1, kWatchdogMask, 0),
        kTag,
        "disable charger watchdog");

    ESP_RETURN_ON_ERROR(
        write_register(kRegVreg, encode_vreg(BOARD_CHARGER_VREG_MV)),
        kTag,
        "set charge voltage");

    uint8_t current_limit = 0;
    ESP_RETURN_ON_ERROR(
        read_register(kRegIchg, &current_limit),
        kTag,
        "read charge current register");
    current_limit = static_cast<uint8_t>(
        (current_limit & kEnIlimMask) |
        encode_ichg(BOARD_CHARGER_ICHG_MA));
    current_limit &= static_cast<uint8_t>(~kEnHizMask);
    ESP_RETURN_ON_ERROR(
        write_register(kRegIchg, current_limit),
        kTag,
        "set charge current");

    uint8_t input_limit = 0;
    ESP_RETURN_ON_ERROR(
        read_register(kRegIindpm, &input_limit),
        kTag,
        "read input current register");
    input_limit = static_cast<uint8_t>(
        (input_limit & 0x20) |
        encode_iindpm(BOARD_CHARGER_IINDPM_MA));
    ESP_RETURN_ON_ERROR(
        write_register(kRegIindpm, input_limit),
        kTag,
        "set input current limit");

    const uint8_t precharge_termination = static_cast<uint8_t>(
        (encode_precharge_or_termination(BOARD_CHARGER_PRECHARGE_MA) << 4) |
        encode_precharge_or_termination(BOARD_CHARGER_TERMINATION_MA));
    ESP_RETURN_ON_ERROR(
        write_register(kRegPrechargeTermination, precharge_termination),
        kTag,
        "set pre-charge and termination current");

    ESP_RETURN_ON_ERROR(
        write_register(kRegAdcDisable, 0x00),
        kTag,
        "enable ADC channels");
    ESP_RETURN_ON_ERROR(
        write_register(kRegAdcControl, kAdcEnabledContinuous12Bit),
        kTag,
        "enable continuous ADC");

    // Measure and validate the battery while both charge-enable controls are
    // still off. This prevents a normal 1S pack from being exposed to the
    // SGM41529 2S regulation voltage.
    vTaskDelay(pdMS_TO_TICKS(250));
    ESP_RETURN_ON_ERROR(
        read_snapshot(),
        kTag,
        "verify disabled charger profile");
    log_register_snapshot("pre-enable");
    if (!s_state.configuration_valid) {
        std::snprintf(
            s_state.detail,
            sizeof(s_state.detail),
            "Charger register verification failed");
        disable_charge_best_effort();
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_state.battery_voltage_safe) {
        disable_charge_best_effort();
        ESP_LOGE(
            kTag,
            "Charging blocked: VBAT=%dmV is outside safe 2S range %d..%dmV%s",
            s_state.vbat_mv,
            BOARD_CHARGER_MIN_SAFE_2S_MV,
            BOARD_CHARGER_MAX_SAFE_2S_MV,
            s_state.single_cell_suspected ? " (possible 1S battery)" : "");
        return ESP_OK;
    }

#if BOARD_CHARGER_ENABLE_ON_BOOT
    ESP_RETURN_ON_ERROR(
        update_register(
            kRegChargerControl2,
            kEnChargeMask,
            kEnChargeMask),
        kTag,
        "enable charging in register");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(
        set_charge_pin_enabled(true),
        kTag,
        "assert charger nCE");
#else
    ESP_RETURN_ON_ERROR(
        set_charge_pin_enabled(false),
        kTag,
        "keep charger nCE disabled");
#endif

    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_RETURN_ON_ERROR(
        read_snapshot(),
        kTag,
        "verify enabled charger profile");
    log_register_snapshot("post-enable");

#if BOARD_CHARGER_ENABLE_ON_BOOT
    if (!s_state.configuration_valid ||
        !s_state.battery_voltage_safe ||
        !s_state.charge_pin_enabled ||
        !s_state.register_charge_enabled ||
        s_state.high_impedance_mode) {
        const bool nce_enabled = s_state.charge_pin_enabled;
        const bool en_chg = s_state.register_charge_enabled;
        const bool en_hiz = s_state.high_impedance_mode;
        disable_charge_best_effort();
        std::snprintf(
            s_state.detail,
            sizeof(s_state.detail),
            "Charge enable failed: nCE=%d EN=%d HIZ=%d",
            nce_enabled,
            en_chg,
            en_hiz);
        ESP_LOGE(kTag, "%s", s_state.detail);
        return ESP_ERR_INVALID_STATE;
    }
#endif

    ESP_LOGI(
        kTag,
        "SGM41529 PN=%u REV=%u configured: VREG=%dmV ICHG=%dmA IINDPM=%dmA",
        s_state.part_id,
        s_state.revision,
        BOARD_CHARGER_VREG_MV,
        BOARD_CHARGER_ICHG_MA,
        BOARD_CHARGER_IINDPM_MA);
    return ESP_OK;
}

esp_err_t cap_detected_input_current(uint8_t *input_limit_register)
{
    const uint8_t configured_code = encode_iindpm(BOARD_CHARGER_IINDPM_MA);
    const uint8_t detected_code = *input_limit_register & 0x1F;
    if (detected_code <= configured_code) {
        return ESP_OK;
    }

    // D+/D- source detection can rewrite IINDPM after initialization. Keep
    // EN_ICO, clear the one-shot FORCE bits, and restore the configured cap.
    const uint8_t capped_register = static_cast<uint8_t>(
        (*input_limit_register & 0x20) | configured_code);
    ESP_RETURN_ON_ERROR(
        write_register(kRegIindpm, capped_register),
        kTag,
        "restore configured input current cap");
    ESP_LOGI(
        kTag,
        "Input detection selected %dmA; capped at %dmA",
        decode_iindpm(detected_code),
        BOARD_CHARGER_IINDPM_MA);
    *input_limit_register = capped_register;
    return ESP_OK;
}

void update_detail_text()
{
    if ((s_state.fault_bits & 0x80) != 0) {
        std::snprintf(s_state.detail, sizeof(s_state.detail), "VBUS over-voltage fault");
    } else if ((s_state.fault_bits & 0x40) != 0) {
        std::snprintf(s_state.detail, sizeof(s_state.detail), "Charger thermal shutdown");
    } else if ((s_state.fault_bits & 0x20) != 0) {
        std::snprintf(s_state.detail, sizeof(s_state.detail), "Battery over-voltage fault");
    } else if ((s_state.fault_bits & 0x10) != 0) {
        std::snprintf(s_state.detail, sizeof(s_state.detail), "Charge safety timer expired");
    } else if (!s_state.configuration_valid) {
        std::snprintf(s_state.detail, sizeof(s_state.detail), "Charger register verification failed");
    } else if (!s_state.battery_voltage_safe && s_state.single_cell_suspected) {
        std::snprintf(s_state.detail, sizeof(s_state.detail), "Possible 1S battery; use protected 2S");
    } else if (!s_state.battery_voltage_safe &&
               s_state.vbat_mv < BOARD_CHARGER_MIN_SAFE_2S_MV) {
        std::snprintf(s_state.detail, sizeof(s_state.detail), "2S voltage too low; charging blocked");
    } else if (!s_state.battery_voltage_safe) {
        std::snprintf(s_state.detail, sizeof(s_state.detail), "Battery voltage too high; charge blocked");
    } else if (s_state.ntc_state == NtcState::kHot ||
               s_state.ntc_state == NtcState::kCold) {
        std::snprintf(s_state.detail, sizeof(s_state.detail), "Battery temperature protection");
    } else if (s_state.thermal_regulation) {
        std::snprintf(s_state.detail, sizeof(s_state.detail), "Thermal regulation reducing current");
    } else if (s_state.input_current_regulation ||
               s_state.input_voltage_regulation) {
        std::snprintf(s_state.detail, sizeof(s_state.detail), "Input DPM reducing charge current");
    } else if (!s_state.power_good) {
        std::snprintf(s_state.detail, sizeof(s_state.detail), "Waiting for a valid USB input");
    } else if (s_state.high_impedance_mode) {
        std::snprintf(s_state.detail, sizeof(s_state.detail), "EN_HIZ=1; charge input disabled");
    } else if (!s_state.register_charge_enabled) {
        std::snprintf(s_state.detail, sizeof(s_state.detail), "EN_CHG=0; charge path disabled");
    } else if (!s_state.charge_pin_enabled) {
        std::snprintf(s_state.detail, sizeof(s_state.detail), "nCE is high; check XL9555 P02");
    } else if (s_state.phase == ChargePhase::kDone) {
        std::snprintf(s_state.detail, sizeof(s_state.detail), "Charge complete");
    } else if (s_state.phase == ChargePhase::kNotCharging) {
        std::snprintf(s_state.detail, sizeof(s_state.detail), "Enabled but idle; inspect raw register log");
    } else {
        std::snprintf(s_state.detail, sizeof(s_state.detail), "Charging normally");
    }
}

esp_err_t read_snapshot()
{
    ChargerRawSnapshot raw = {};
    ESP_RETURN_ON_ERROR(
        read_registers(
            kRegVreg,
            raw.control.data(),
            raw.control.size()),
        kTag,
        "read charger registers");
    ESP_RETURN_ON_ERROR(
        read_registers(
            kRegAdcControl,
            raw.adc.data(),
            raw.adc.size()),
        kTag,
        "read charger ADC registers");
    ESP_RETURN_ON_ERROR(
        cap_detected_input_current(&raw.control[kRegIindpm]),
        kTag,
        "enforce input current cap");

    ESP_RETURN_ON_ERROR(
        esp_io_expander_get_level(
            s_io_expander,
            0xFFFF,
            &raw.io_levels),
        kTag,
        "read charger status pins");
    s_raw_snapshot = raw;

    const uint8_t status1 = raw.control[kRegStatus1];
    const uint8_t status2 = raw.control[kRegStatus2];
    const uint8_t ntc_status = raw.control[kRegNtcStatus] & 0x07;
    const uint8_t part_information = raw.adc[0x10];

    s_state.data_valid = true;
    s_state.device_present = true;
    s_state.part_id = (part_information >> 3) & 0x0F;
    s_state.revision = part_information & 0x07;
    s_state.part_id_valid = s_state.part_id == kExpectedPartId;
    s_state.charge_pin_enabled =
        (raw.io_levels & BOARD_XL9555_P02_CHARGE_EN) == 0;
    s_state.register_charge_enabled =
        (raw.control[kRegChargerControl2] & kEnChargeMask) != 0;
    s_state.high_impedance_mode =
        (raw.control[kRegIchg] & kEnHizMask) != 0;
    s_state.charge_enabled =
        s_state.charge_pin_enabled &&
        s_state.register_charge_enabled &&
        !s_state.high_impedance_mode;
    s_state.adc_enabled = (raw.adc[0] & 0x80) != 0;
    s_state.power_good = (status2 & 0x80) != 0;
    s_state.power_good_pin =
        (raw.io_levels & BOARD_XL9555_P04_CHARGE_GOOD) == 0;
    s_state.interrupt_pin_active =
        (raw.io_levels & BOARD_XL9555_P03_CHARGE_INT) == 0;
    s_state.input_current_regulation = (status1 & 0x40) != 0;
    s_state.input_voltage_regulation = (status1 & 0x20) != 0;
    s_state.thermal_regulation = (status1 & 0x10) != 0;
    s_state.watchdog_expired = (status1 & 0x08) != 0;
    s_state.minimum_system_regulation = (status2 & 0x01) != 0;
    s_state.phase = static_cast<ChargePhase>(status1 & 0x07);
    s_state.input_source = static_cast<InputSource>((status2 >> 4) & 0x07);
    s_state.ico_status = (status2 >> 1) & 0x03;
    switch (ntc_status) {
    case 0:
        s_state.ntc_state = NtcState::kNormal;
        break;
    case 2:
        s_state.ntc_state = NtcState::kWarm;
        break;
    case 3:
        s_state.ntc_state = NtcState::kCool;
        break;
    case 5:
        s_state.ntc_state = NtcState::kCold;
        break;
    case 6:
        s_state.ntc_state = NtcState::kHot;
        break;
    default:
        s_state.ntc_state = NtcState::kUnknown;
        break;
    }
    s_state.fault_bits = raw.control[kRegFaultStatus] & 0xF0;

    s_state.ibus_ma = decode_ibus(raw.adc[2], raw.adc[3]);
    s_state.ichg_ma = decode_adc_unsigned(raw.adc[4], raw.adc[5], 0x0F);
    s_state.vbus_mv = decode_adc_unsigned(raw.adc[6], raw.adc[7], 0x1F);
    s_state.vbat_mv = decode_adc_unsigned(raw.adc[8], raw.adc[9], 0x3F);
    s_state.vsys_mv = decode_adc_unsigned(raw.adc[10], raw.adc[11], 0x3F);
    const int ts_raw = decode_adc_unsigned(raw.adc[12], raw.adc[13], 0x03);
    const int die_raw = decode_adc_unsigned(raw.adc[14], raw.adc[15], 0x01);
    s_state.ts_deci_percent = (ts_raw * 98 + 50) / 100;
    s_state.die_temperature_deci_c = die_raw * 5;

    s_state.voltage_limit_mv = decode_vreg(raw.control[kRegVreg]);
    s_state.charge_current_limit_ma = decode_ichg(raw.control[kRegIchg]);
    s_state.precharge_current_ma =
        decode_precharge_or_termination(
            raw.control[kRegPrechargeTermination] >> 4);
    s_state.termination_current_ma =
        decode_precharge_or_termination(
            raw.control[kRegPrechargeTermination]);
    s_state.thermal_regulation_limit_c =
        decode_thermal_regulation(raw.control[kRegChargerControl2]);
    if (s_state.ico_status == 1 || s_state.ico_status == 2) {
        s_state.input_current_limit_ma = decode_iindpm(raw.control[0x0A]);
    } else {
        s_state.input_current_limit_ma =
            decode_iindpm(raw.control[kRegIindpm]);
    }

    s_state.configuration_valid = profile_matches(raw);
    s_state.battery_voltage_safe =
        s_state.vbat_mv >= BOARD_CHARGER_MIN_SAFE_2S_MV &&
        s_state.vbat_mv <= BOARD_CHARGER_MAX_SAFE_2S_MV;
    s_state.single_cell_suspected =
        s_state.vbat_mv >= kLikelySingleCellMinMv &&
        s_state.vbat_mv <= kLikelySingleCellMaxMv;

    update_detail_text();
    return ESP_OK;
}

void charger_task(void *)
{
    bool configured = false;
    int read_failures = 0;
    unsigned sample_count = 0;
    ChargePhase last_phase = ChargePhase::kUnknown;
    uint8_t last_fault_bits = 0xFF;
    bool last_power_good = false;
    bool last_battery_voltage_safe = false;
    bool last_charge_enabled = false;

    while (true) {
        if (!configured) {
            s_state.data_valid = false;
            const esp_err_t configure_err = configure_charger();
            if (configure_err != ESP_OK) {
                disable_charge_best_effort();
                publish_state();
                ESP_LOGW(
                    kTag,
                    "Charger initialization pending: %s",
                    esp_err_to_name(configure_err));
                vTaskDelay(pdMS_TO_TICKS(kRetryDelayMs));
                continue;
            }
            configured = true;
            read_failures = 0;
        }

        const esp_err_t read_err = read_snapshot();
        if (read_err != ESP_OK) {
            ++read_failures;
            s_state.data_valid = false;
            std::snprintf(
                s_state.detail,
                sizeof(s_state.detail),
                "SGM41529 read failed (%d/%d)",
                read_failures,
                kReadFailureLimit);
            ESP_LOGE(kTag, "%s: %s", s_state.detail, esp_err_to_name(read_err));

            if (read_failures >= kReadFailureLimit) {
                disable_charge_best_effort();
                configured = false;
                s_state.charge_enabled = false;
                std::snprintf(
                    s_state.detail,
                    sizeof(s_state.detail),
                    "Monitor lost; charge disabled");
            }
            publish_state();
        } else {
            read_failures = 0;

            if (!s_state.battery_voltage_safe && s_state.charge_enabled) {
                ESP_LOGE(
                    kTag,
                    "VBAT left the safe 2S range while charging; disabling immediately");
                disable_charge_best_effort();
                update_detail_text();
            }

            const bool charge_enable_lost =
                BOARD_CHARGER_ENABLE_ON_BOOT &&
                s_state.battery_voltage_safe &&
                (!s_state.charge_pin_enabled ||
                 !s_state.register_charge_enabled ||
                 s_state.high_impedance_mode);
            const bool configuration_lost =
                !s_state.part_id_valid ||
                s_state.watchdog_expired ||
                !s_state.adc_enabled ||
                !s_state.configuration_valid ||
                charge_enable_lost;
            if (configuration_lost) {
                const bool part_id_valid = s_state.part_id_valid;
                const bool watchdog_expired = s_state.watchdog_expired;
                const bool adc_enabled = s_state.adc_enabled;
                const bool profile_valid = s_state.configuration_valid;
                const bool nce_enabled = s_state.charge_pin_enabled;
                const bool en_chg = s_state.register_charge_enabled;
                const bool en_hiz = s_state.high_impedance_mode;
                disable_charge_best_effort();
                configured = false;
                s_state.charge_enabled = false;
                s_state.data_valid = false;
                if (!part_id_valid) {
                    std::snprintf(
                        s_state.detail,
                        sizeof(s_state.detail),
                        "Unexpected charger ID: PN=%u REV=%u",
                        s_state.part_id,
                        s_state.revision);
                } else if (watchdog_expired) {
                    std::snprintf(
                        s_state.detail,
                        sizeof(s_state.detail),
                        "Watchdog reset; charge disabled");
                } else if (!adc_enabled) {
                    std::snprintf(
                        s_state.detail,
                        sizeof(s_state.detail),
                        "ADC stopped; charge disabled");
                } else if (!profile_valid) {
                    std::snprintf(
                        s_state.detail,
                        sizeof(s_state.detail),
                        "Charger register drift; charge disabled");
                } else {
                    std::snprintf(
                        s_state.detail,
                        sizeof(s_state.detail),
                        "Charge gate lost: nCE=%d EN=%d HIZ=%d",
                        nce_enabled,
                        en_chg,
                        en_hiz);
                }
                publish_state();
                log_register_snapshot("configuration-lost");
                ESP_LOGW(kTag, "Charger configuration lost; reinitializing");
                vTaskDelay(pdMS_TO_TICKS(kRetryDelayMs));
                continue;
            }

            publish_state();

            const bool state_changed =
                s_state.phase != last_phase ||
                s_state.fault_bits != last_fault_bits ||
                s_state.power_good != last_power_good ||
                s_state.battery_voltage_safe != last_battery_voltage_safe ||
                s_state.charge_enabled != last_charge_enabled;
            if (state_changed || (sample_count % 10U) == 0U) {
                ESP_LOGI(
                    kTag,
                    "phase=%u PG=%d VBUS=%dmV IBUS=%dmA VBAT=%dmV ICHG=%dmA "
                    "VSYS=%dmV TDIE=%d.%dC NTC=%u CE=%d/%d HIZ=%d faults=0x%02X",
                    static_cast<unsigned>(s_state.phase),
                    s_state.power_good,
                    s_state.vbus_mv,
                    s_state.ibus_ma,
                    s_state.vbat_mv,
                    s_state.ichg_ma,
                    s_state.vsys_mv,
                    s_state.die_temperature_deci_c / 10,
                    s_state.die_temperature_deci_c % 10,
                    static_cast<unsigned>(s_state.ntc_state),
                    s_state.charge_pin_enabled,
                    s_state.register_charge_enabled,
                    s_state.high_impedance_mode,
                    s_state.fault_bits);
                log_register_snapshot("status");
            }
            last_phase = s_state.phase;
            last_fault_bits = s_state.fault_bits;
            last_power_good = s_state.power_good;
            last_battery_voltage_safe = s_state.battery_voltage_safe;
            last_charge_enabled = s_state.charge_enabled;
            ++sample_count;
        }

        vTaskDelay(pdMS_TO_TICKS(BOARD_CHARGER_SAMPLE_PERIOD_MS));
    }
}

}  // namespace

esp_err_t charger_test_start(
    ChargerStateCallback callback,
    void *callback_context)
{
    ESP_RETURN_ON_FALSE(
        callback != nullptr,
        ESP_ERR_INVALID_ARG,
        kTag,
        "null charger callback");
    ESP_RETURN_ON_FALSE(
        !s_task_started,
        ESP_ERR_INVALID_STATE,
        kTag,
        "charger task already started");

    s_callback = callback;
    s_callback_context = callback_context;
    std::snprintf(
        s_state.detail,
        sizeof(s_state.detail),
        "Initializing SGM41529 at 0x%02X",
        BOARD_CHARGER_I2C_ADDR);
    publish_state();

    const esp_err_t init_err = init_i2c_and_expander();
    if (init_err != ESP_OK) {
        disable_charge_best_effort();
        s_state.data_valid = false;
        s_state.device_present = false;
        std::snprintf(
            s_state.detail,
            sizeof(s_state.detail),
            "Charger startup failed: %s",
            esp_err_to_name(init_err));
        publish_state();
        ESP_LOGE(
            kTag,
            "initialize charger control bus: %s",
            esp_err_to_name(init_err));
        return init_err;
    }

    if (xTaskCreate(
            charger_task,
            "charger_monitor",
            4096,
            nullptr,
            5,
            nullptr) != pdPASS) {
        disable_charge_best_effort();
        s_state.data_valid = false;
        std::snprintf(
            s_state.detail,
            sizeof(s_state.detail),
            "Unable to start charger monitor task");
        publish_state();
        return ESP_ERR_NO_MEM;
    }

    s_task_started = true;
    return ESP_OK;
}
