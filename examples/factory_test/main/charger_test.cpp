#include "factory_tests.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>

#include "board_config.h"
#include "board_service.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "factory_settings.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr char kTag[] = "factory_charger";
constexpr int kTimeoutMs = 100;
constexpr uint8_t kExpectedPartId = 0x03;
constexpr uint8_t kRegVreg = 0x00;
constexpr uint8_t kRegIchg = 0x01;
constexpr uint8_t kRegIindpm = 0x03;
constexpr uint8_t kRegPreTerm = 0x04;
constexpr uint8_t kRegControl1 = 0x05;
constexpr uint8_t kRegControl2 = 0x06;
constexpr uint8_t kRegControl3 = 0x07;
constexpr uint8_t kRegStatus1 = 0x0B;
constexpr uint8_t kRegStatus2 = 0x0C;
constexpr uint8_t kRegNtc = 0x0D;
constexpr uint8_t kRegFault = 0x0E;
constexpr uint8_t kRegAdc = 0x15;
constexpr uint8_t kRegAdcDisable = 0x16;
constexpr uint8_t kRegPart = 0x25;
constexpr uint8_t kEnHiz = 0x80;
constexpr uint8_t kWatchdogMask = 0x30;
constexpr uint8_t kEnOtg = 0x80;
constexpr uint8_t kThermalMask = 0x30;
constexpr uint8_t kEnCharge = 0x08;
constexpr uint8_t kWatchdogReset = 0x40;

static_assert(
    FACTORY_CHARGER_VREG_MV >= 6800 &&
        FACTORY_CHARGER_VREG_MV <= 9200 &&
        FACTORY_CHARGER_VREG_MV % 10 == 0,
    "Charger voltage must be 6800..9200 mV in 10 mV steps");
static_assert(
    FACTORY_CHARGER_CURRENT_MA >= 100 &&
        FACTORY_CHARGER_CURRENT_MA <= 2200 &&
        FACTORY_CHARGER_CURRENT_MA % 50 == 0,
    "Charge current must be 100..2200 mA in 50 mA steps");

i2c_master_dev_handle_t s_charger = nullptr;

struct Snapshot {
    std::array<uint8_t, 0x0F> reg = {};
    std::array<uint8_t, 0x11> adc = {};
    uint32_t io = 0;
    int vbus_mv = 0;
    int vbat_mv = 0;
    int ichg_ma = 0;
    int ibus_ma = 0;
    uint8_t phase = 0;
    uint8_t ntc = 0;
    uint8_t faults = 0;
    uint8_t part = 0;
    uint8_t revision = 0;
    bool power_good = false;
    bool power_good_pin = false;
    bool nce_enabled = false;
    bool register_charge_enabled = false;
    bool high_impedance_mode = false;
    bool watchdog_expired = false;
    bool adc_enabled = false;
    bool profile_ok = false;
};

esp_err_t read_regs(uint8_t first, uint8_t *data, size_t length)
{
    return i2c_master_transmit_receive(
        s_charger, &first, 1, data, length, kTimeoutMs);
}

esp_err_t read_reg(uint8_t reg, uint8_t *value)
{
    return read_regs(reg, value, 1);
}

esp_err_t write_reg(uint8_t reg, uint8_t value)
{
    const uint8_t command[] = {reg, value};
    return i2c_master_transmit(s_charger, command, sizeof(command), kTimeoutMs);
}

esp_err_t update_reg(uint8_t reg, uint8_t mask, uint8_t value)
{
    uint8_t current = 0;
    esp_err_t err = read_reg(reg, &current);
    if (err == ESP_OK) {
        current = static_cast<uint8_t>((current & ~mask) | (value & mask));
        err = write_reg(reg, current);
    }
    return err;
}

int decode_unsigned(uint8_t high, uint8_t low, uint8_t high_mask)
{
    return ((high & high_mask) << 8) | low;
}

int decode_ibus(uint8_t high, uint8_t low)
{
    const int value = decode_unsigned(high, low, 0x0F);
    return (high & 0x80) != 0 ? -value : value;
}

esp_err_t read_snapshot(Snapshot *snapshot)
{
    if (snapshot == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *snapshot = {};
    esp_err_t err = read_regs(kRegVreg, snapshot->reg.data(), snapshot->reg.size());
    if (err == ESP_OK) {
        err = read_regs(kRegAdc, snapshot->adc.data(), snapshot->adc.size());
    }
    if (err == ESP_OK) {
        err = board_service_get_level(0xFFFF, &snapshot->io);
    }
    if (err != ESP_OK) {
        return err;
    }
    snapshot->vbus_mv = decode_unsigned(snapshot->adc[6], snapshot->adc[7], 0x1F);
    snapshot->vbat_mv = decode_unsigned(snapshot->adc[8], snapshot->adc[9], 0x3F);
    snapshot->ichg_ma = decode_unsigned(snapshot->adc[4], snapshot->adc[5], 0x0F);
    snapshot->ibus_ma = decode_ibus(snapshot->adc[2], snapshot->adc[3]);
    snapshot->phase = snapshot->reg[kRegStatus1] & 0x07;
    snapshot->ntc = snapshot->reg[kRegNtc] & 0x07;
    snapshot->faults = snapshot->reg[kRegFault] & 0xF0;
    snapshot->part = (snapshot->adc[0x10] >> 3) & 0x0F;
    snapshot->revision = snapshot->adc[0x10] & 0x07;
    snapshot->power_good = (snapshot->reg[kRegStatus2] & 0x80) != 0;
    snapshot->power_good_pin =
        (snapshot->io & BOARD_XL_P04_CHARGE_PG) == 0;
    snapshot->nce_enabled =
        (snapshot->io & BOARD_XL_P02_CHARGE_NCE) == 0;
    snapshot->register_charge_enabled =
        (snapshot->reg[kRegControl2] & kEnCharge) != 0;
    snapshot->high_impedance_mode =
        (snapshot->reg[kRegIchg] & kEnHiz) != 0;
    snapshot->watchdog_expired =
        (snapshot->reg[kRegStatus1] & 0x08) != 0;
    snapshot->adc_enabled = (snapshot->adc[0] & 0x80) != 0;
    const uint8_t expected_vreg =
        static_cast<uint8_t>((FACTORY_CHARGER_VREG_MV - 6800) / 10);
    const uint8_t expected_ichg = FACTORY_CHARGER_CURRENT_MA / 50;
    snapshot->profile_ok =
        snapshot->reg[kRegVreg] == expected_vreg &&
        (snapshot->reg[kRegIchg] & 0xBF) == expected_ichg &&
        (snapshot->reg[kRegIindpm] & 0x1F) <= 5 &&
        snapshot->reg[kRegPreTerm] == 0x11 &&
        (snapshot->reg[kRegControl1] & kWatchdogMask) == 0 &&
        (snapshot->reg[kRegControl2] & (kEnOtg | kThermalMask)) == 0x10 &&
        (snapshot->reg[kRegControl3] & kWatchdogReset) == 0 &&
        (snapshot->adc[0] & 0xF0) == 0xB0 &&
        snapshot->adc_enabled &&
        snapshot->part == kExpectedPartId;
    return ESP_OK;
}

esp_err_t disable_charge()
{
    const esp_err_t pin_err =
        board_service_set_output(BOARD_XL_P02_CHARGE_NCE, true);
    esp_err_t register_err = ESP_OK;
    if (s_charger != nullptr) {
        register_err = update_reg(kRegControl2, kEnCharge, 0);
    }
    if (pin_err != ESP_OK) {
        ESP_LOGE(kTag, "Unable to deassert charger nCE: %s", esp_err_to_name(pin_err));
    }
    if (register_err != ESP_OK) {
        ESP_LOGE(kTag, "Unable to clear charger EN_CHG: %s", esp_err_to_name(register_err));
    }
    return pin_err != ESP_OK ? pin_err : register_err;
}

esp_err_t configure_charger()
{
    ESP_RETURN_ON_ERROR(
        disable_charge(), kTag, "disable both charger gates before configuration");
    uint8_t part = 0;
    esp_err_t err = read_reg(kRegPart, &part);
    if (err != ESP_OK || ((part >> 3) & 0x0F) != kExpectedPartId) {
        return err == ESP_OK ? ESP_ERR_INVALID_RESPONSE : err;
    }
    err = update_reg(
        kRegControl2, kEnOtg | kThermalMask | kEnCharge, 0x10);
    if (err == ESP_OK) {
        err = update_reg(kRegControl3, kWatchdogReset, kWatchdogReset);
    }
    if (err == ESP_OK) {
        err = update_reg(kRegControl1, kWatchdogMask, 0);
    }
    if (err == ESP_OK) {
        err = write_reg(
            kRegVreg,
            static_cast<uint8_t>((FACTORY_CHARGER_VREG_MV - 6800) / 10));
    }
    uint8_t current = 0;
    if (err == ESP_OK) {
        err = read_reg(kRegIchg, &current);
    }
    if (err == ESP_OK) {
        current = static_cast<uint8_t>(
            (current & 0x40) | (FACTORY_CHARGER_CURRENT_MA / 50));
        current &= static_cast<uint8_t>(~kEnHiz);
        err = write_reg(kRegIchg, current);
    }
    uint8_t input = 0;
    if (err == ESP_OK) {
        err = read_reg(kRegIindpm, &input);
    }
    if (err == ESP_OK) {
        err = write_reg(kRegIindpm, static_cast<uint8_t>((input & 0x20) | 5));
    }
    if (err == ESP_OK) {
        err = write_reg(kRegPreTerm, 0x11);
    }
    if (err == ESP_OK) {
        err = write_reg(kRegAdcDisable, 0x00);
    }
    if (err == ESP_OK) {
        err = write_reg(kRegAdc, 0xB0);
    }
    return err;
}

void set_failure(FactoryTestRecord *record, const char *code, const char *detail)
{
    std::snprintf(record->error_code, sizeof(record->error_code), "%s", code);
    std::snprintf(record->detail, sizeof(record->detail), "%s", detail);
}

bool ntc_is_safe(const Snapshot &sample)
{
    return sample.ntc == 0 || sample.ntc == 2 || sample.ntc == 3;
}

bool battery_is_safe(const Snapshot &sample)
{
    return sample.vbat_mv >= FACTORY_CHARGER_VBAT_MIN_MV &&
           sample.vbat_mv <= FACTORY_CHARGER_VBAT_MAX_MV;
}

void store_measurements(
    FactoryTestRecord *record,
    const Snapshot &sample,
    bool include_shutdown = false,
    bool shutdown_ok = false)
{
    const char *shutdown = include_shutdown ?
        (shutdown_ok ? ",\"disabled_after_test\":true" :
                       ",\"disabled_after_test\":false") : "";
    std::snprintf(
        record->measurements, sizeof(record->measurements),
        "{\"part_id\":%u,\"revision\":%u,\"vbus_mv\":%d,\"vbat_mv\":%d,"
        "\"ibus_ma\":%d,\"ichg_ma\":%d,\"phase\":%u,\"ntc\":%u,"
        "\"fault_bits\":%u,\"power_good\":%s,\"power_good_pin\":%s,"
        "\"nce_enabled\":%s,\"en_chg\":%s,\"en_hiz\":%s,"
        "\"profile_readback\":%s%s}",
        sample.part, sample.revision, sample.vbus_mv, sample.vbat_mv,
        sample.ibus_ma, sample.ichg_ma, sample.phase, sample.ntc,
        sample.faults, sample.power_good ? "true" : "false",
        sample.power_good_pin ? "true" : "false",
        sample.nce_enabled ? "true" : "false",
        sample.register_charge_enabled ? "true" : "false",
        sample.high_impedance_mode ? "true" : "false",
        sample.profile_ok ? "true" : "false", shutdown);
}

}  // namespace

esp_err_t charger_test_run(
    const FactoryTestEnvironment &,
    FactoryTestRecord *record,
    FactoryModuleOutcome *outcome)
{
    if (record == nullptr || outcome == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *outcome = {};
    charger_test_cleanup();
    const FactoryBoardState &board = board_service_state();
    if (!board.i2c_ready || !board.expander_ready) {
        set_failure(record, "I2C_NO_ACK", "I2C0 or XL9555 unavailable");
        return ESP_ERR_NOT_FOUND;
    }
    i2c_device_config_t config = {};
    config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    config.device_address = BOARD_CHARGER_ADDR;
    config.scl_speed_hz = 100000;
    esp_err_t err = i2c_master_bus_add_device(board.i2c_bus, &config, &s_charger);
    if (err != ESP_OK) {
        set_failure(record, "I2C_NO_ACK", "Unable to add SGM41529");
        return err;
    }
    board_ui_show_status("CHARGER", "Configuring 8.4 V / 500 mA", "Charging remains disabled during safety checks");
    err = configure_charger();
    if (err != ESP_OK) {
        disable_charge();
        set_failure(record, "PART_ID_MISMATCH", "SGM41529 identification/configuration failed");
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(250));
    Snapshot sample = {};
    err = read_snapshot(&sample);
    if (err != ESP_OK || !sample.profile_ok || sample.nce_enabled ||
        sample.register_charge_enabled || sample.high_impedance_mode ||
        sample.watchdog_expired) {
        disable_charge();
        if (err == ESP_OK) {
            store_measurements(record, sample);
        }
        set_failure(record, "CONFIG_READBACK", "Charger profile readback failed");
        return err == ESP_OK ? ESP_ERR_INVALID_STATE : err;
    }
    if (!battery_is_safe(sample)) {
        disable_charge();
        set_failure(record, "UNSAFE_BATTERY", "VBAT is outside the protected 2S range");
        store_measurements(record, sample);
        return ESP_ERR_INVALID_STATE;
    }
    if (sample.vbus_mv < FACTORY_CHARGER_VBUS_MIN_MV ||
        !sample.power_good || !sample.power_good_pin) {
        disable_charge();
        set_failure(record, "VBUS_INVALID", "VBUS or power-good check failed");
        store_measurements(record, sample);
        return ESP_ERR_INVALID_STATE;
    }
    if (!ntc_is_safe(sample) || sample.faults != 0) {
        disable_charge();
        set_failure(record, "CHARGER_FAULT", "NTC or charger fault status is unsafe");
        store_measurements(record, sample);
        return ESP_ERR_INVALID_STATE;
    }

    err = update_reg(kRegControl2, kEnCharge, kEnCharge);
    if (err == ESP_OK) {
        err = board_service_set_output(BOARD_XL_P02_CHARGE_NCE, false);
    }
    if (err != ESP_OK) {
        disable_charge();
        set_failure(record, "ENABLE_FAILED", "Unable to enable both charge gates");
        return err;
    }
    bool charge_done = false;
    bool active_charge = false;
    bool positive_current = false;
    for (int sample_index = 0; sample_index < 10; ++sample_index) {
        vTaskDelay(pdMS_TO_TICKS(500));
        if (read_snapshot(&sample) != ESP_OK) {
            disable_charge();
            set_failure(record, "I2C_READ_ERROR", "Lost charger telemetry");
            return ESP_FAIL;
        }
        if (!battery_is_safe(sample)) {
            disable_charge();
            store_measurements(record, sample);
            set_failure(record, "UNSAFE_BATTERY", "VBAT left the protected 2S range");
            return ESP_ERR_INVALID_STATE;
        }
        if (sample.vbus_mv < FACTORY_CHARGER_VBUS_MIN_MV ||
            !sample.power_good || !sample.power_good_pin) {
            disable_charge();
            store_measurements(record, sample);
            set_failure(record, "VBUS_INVALID", "VBUS or power-good was lost while charging");
            return ESP_ERR_INVALID_STATE;
        }
        if (!ntc_is_safe(sample) || sample.faults != 0) {
            disable_charge();
            store_measurements(record, sample);
            set_failure(record, "CHARGER_FAULT", "NTC or charger fault became unsafe");
            return ESP_ERR_INVALID_STATE;
        }
        const bool gates_enabled =
            sample.nce_enabled && sample.register_charge_enabled &&
            !sample.high_impedance_mode;
        if (!sample.profile_ok || sample.watchdog_expired || !gates_enabled) {
            disable_charge();
            store_measurements(record, sample);
            set_failure(record, "CONFIG_READBACK", "Charge gate or profile changed during the test");
            return ESP_ERR_INVALID_STATE;
        }
        char detail[96] = {};
        std::snprintf(detail, sizeof(detail), "VBUS %d mV  VBAT %d mV  ICHG %d mA",
                      sample.vbus_mv, sample.vbat_mv, sample.ichg_ma);
        board_ui_show_status("CHARGER", detail, "Short charge-path test (maximum 5 s)");
        charge_done = sample.phase == 6;
        active_charge = sample.phase >= 1 && sample.phase <= 5;
        positive_current = sample.ichg_ma > 0;
        if (charge_done || (active_charge && positive_current)) {
            break;
        }
    }
    const Snapshot active_sample = sample;
    const esp_err_t disable_err = disable_charge();
    vTaskDelay(pdMS_TO_TICKS(20));
    Snapshot disabled_sample = {};
    const esp_err_t shutdown_read_err =
        disable_err == ESP_OK ? read_snapshot(&disabled_sample) : disable_err;
    const bool shutdown_ok =
        shutdown_read_err == ESP_OK &&
        !disabled_sample.nce_enabled &&
        !disabled_sample.register_charge_enabled;
    const bool final_ntc_ok = ntc_is_safe(sample);
    const bool active_gates_ok =
        sample.nce_enabled && sample.register_charge_enabled &&
        !sample.high_impedance_mode;
    outcome->automatic_pass = factory_charger_threshold_pass(
        sample.vbus_mv, sample.vbat_mv, sample.ichg_ma,
        charge_done, active_charge, final_ntc_ok, sample.faults) &&
        sample.power_good && sample.power_good_pin && sample.profile_ok &&
        !sample.watchdog_expired && active_gates_ok;
    store_measurements(record, active_sample, true, shutdown_ok);
    if (!shutdown_ok) {
        set_failure(
            record, "CLEANUP_FAILED",
            "Unable to verify both charger gates disabled after the test");
        return shutdown_read_err == ESP_OK ? ESP_ERR_INVALID_STATE :
                                             shutdown_read_err;
    }
    if (!outcome->automatic_pass) {
        set_failure(record, "NO_CHARGE_CURRENT", "No positive charge current and charger is not Done");
        return ESP_FAIL;
    }
    std::snprintf(record->detail, sizeof(record->detail),
                  charge_done ? "Charger reports Done" : "Positive charging current detected");
    return ESP_OK;
}

void charger_test_cleanup()
{
    disable_charge();
    if (s_charger != nullptr && board_service_state().i2c_ready) {
        i2c_master_bus_rm_device(s_charger);
    }
    s_charger = nullptr;
}
