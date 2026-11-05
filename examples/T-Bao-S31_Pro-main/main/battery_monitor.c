#include "battery_monitor.h"

#include "battery_estimator.h"
#include "app_language.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_io_expander.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_sleep.h"
#include "esp_xiaozhi_chat_display.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "tbao_s31_board.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "battery_monitor";

enum {
    kI2cTimeoutMs = 100,
    kReadFailureLimit = 3,
    kExpectedPartId = 3,
    kAdcEnabledContinuous12Bit = 0xB0,
    kAdcSettleMs = 250,
    kShutdownCountdownSeconds = 30,
    kProtectionPollMs = 100,
    /* SGM41529 ICHG ADC is 1mA/LSB. Keep the fallback above idle noise while
     * still detecting the 50-80mA trickle/pre-charge ranges. */
    kChargingCurrentThresholdMa = 30,
    kRegIchg = 0x01,
    kRegIindpm = 0x03,
    kRegChargerControl1 = 0x05,
    kRegIcoCurrentLimit = 0x0A,
    kRegStatus1 = 0x0B,
    kRegStatus2 = 0x0C,
    kRegNtcStatus = 0x0D,
    kRegFaultStatus = 0x0E,
    kRegAdcControl = 0x15,
    kRegAdcDisable = 0x16,
    kRegPartInformation = 0x25,
};

/* Only REG03's input-current ceiling is maintained by the monitor. Battery
 * voltage/current, charge gates, ILIM, ICO and other protections are preserved. */
static const uint8_t kEnHizMask = 1U << 7;
static const uint8_t kEnIlimMask = 1U << 6;
static const uint8_t kEnChargeMask = 1U << 3;
static const uint8_t kIindpmMask = 0x1FU;
static const uint8_t kEnIcoMask = 1U << 5;
static const uint8_t kForceInputDetectionMask = 1U << 6;

_Static_assert(BATTERY_MONITOR_INPUT_CURRENT_LIMIT_MA >= 500 &&
                   BATTERY_MONITOR_INPUT_CURRENT_LIMIT_MA <= 3300 &&
                   (BATTERY_MONITOR_INPUT_CURRENT_LIMIT_MA - 500) % 100 == 0,
               "Input current limit must be 500..3300mA in 100mA steps");
_Static_assert(BATTERY_MONITOR_CHARGING_CONFIRM_SAMPLES > 0,
               "Charging confirmation requires at least one fresh sample");

static i2c_master_dev_handle_t s_charger;
static esp_io_expander_handle_t s_expander;
static SemaphoreHandle_t s_state_mutex;
static TaskHandle_t s_task;
static TaskHandle_t s_protection_task;
static TaskHandle_t s_display_task;
static QueueHandle_t s_sample_queue;
static QueueHandle_t s_display_queue;
typedef struct {
    battery_monitor_state_t state;
    esp_err_t error;
    TickType_t sampled_at;
} battery_sample_t;
static battery_monitor_callback_t s_callback;
static void *s_callback_context;
static bool s_started;
static bool s_adc_configured;
static int s_filtered_percent = -1;
static unsigned s_low_voltage_samples;
static unsigned s_shutdown_voltage_samples;
static bool s_low_voltage_warning;
static unsigned s_charging_confirm_samples;
static bool s_shutdown_countdown_active;
static TickType_t s_shutdown_countdown_deadline;
static int s_shutdown_countdown_last_second = -1;
static int s_shutdown_trigger_voltage_mv;
static bool s_protection_sleep_started;
/* The UI only latches a request. The protection worker remains the sole owner
 * of both automatic and manual shutdown, outside the LVGL event callback. */
static portMUX_TYPE s_sleep_request_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_manual_sleep_requested;
static battery_monitor_state_t s_state = {
    .battery_percent = -1,
    .input_current_limit_ma = -1,
    .ico_current_limit_ma = -1,
    .shutdown_seconds_remaining = -1,
    .phase = BATTERY_MONITOR_PHASE_UNKNOWN,
};

static esp_err_t read_registers(uint8_t first_register, uint8_t *data, size_t length)
{
    if (s_charger == NULL || data == NULL || length == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_transmit_receive(s_charger, &first_register, 1,
                                       data, length, kI2cTimeoutMs);
}

static esp_err_t read_register(uint8_t register_address, uint8_t *value)
{
    return read_registers(register_address, value, 1);
}

static esp_err_t write_register(uint8_t register_address, uint8_t value)
{
    const uint8_t command[] = {register_address, value};
    if (s_charger == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_transmit(s_charger, command, sizeof(command),
                               kI2cTimeoutMs);
}

static int decode_adc_unsigned(uint8_t high, uint8_t low, uint8_t high_mask)
{
    return ((int)(high & high_mask) << 8) | low;
}

static int decode_ibus(uint8_t high, uint8_t low)
{
    const int magnitude = decode_adc_unsigned(high, low, 0x0F);
    return (high & 0x80U) != 0 ? -magnitude : magnitude;
}

static int decode_input_current_limit(uint8_t value)
{
    const unsigned code = value & kIindpmMask;
    return 500 + (int)(code > 28U ? 28U : code) * 100;
}

static esp_err_t maintain_input_current_limit(battery_monitor_state_t *state)
{
    uint8_t input_control = 0;
    ESP_RETURN_ON_ERROR(read_register(kRegIindpm, &input_control), TAG,
                        "read input current limit");
    state->input_current_limit_ma = decode_input_current_limit(input_control);

    const uint8_t target_code =
        (BATTERY_MONITOR_INPUT_CURRENT_LIMIT_MA - 500) / 100;
    if ((input_control & kIindpmMask) != target_code &&
        (input_control & kForceInputDetectionMask) == 0) {
        const int previous_limit_ma = state->input_current_limit_ma;
        /* D+/D- detection can overwrite IINDPM after USB insertion. Restore
         * the ceiling each sampling cycle if needed, preserve EN_ICO, and
         * never replay the one-shot FORCE_ICO/FORCE_INDET bits. */
        const uint8_t updated_control =
            (input_control & kEnIcoMask) | target_code;
        ESP_RETURN_ON_ERROR(write_register(kRegIindpm, updated_control), TAG,
                            "set input current limit");
        ESP_RETURN_ON_ERROR(read_register(kRegIindpm, &input_control), TAG,
                            "read back input current limit");
        state->input_current_limit_ma = decode_input_current_limit(input_control);
        ESP_RETURN_ON_FALSE((input_control & kIindpmMask) == target_code,
                            ESP_ERR_INVALID_RESPONSE, TAG,
                            "input current limit was overwritten; retry next sample");
        ESP_LOGI(TAG,
                 "USB input limit: %dmA -> %dmA; REG03=0x%02X "
                 "VBUS_STAT=%u EN_ILIM=%d EN_ICO=%d",
                 previous_limit_ma, state->input_current_limit_ma,
                 input_control, (unsigned)state->input_source,
                 state->input_limit_pin_enabled,
                 (input_control & kEnIcoMask) != 0);
    }

    if (state->ico_status == 1 || state->ico_status == 2) {
        uint8_t ico_limit = 0;
        ESP_RETURN_ON_ERROR(read_register(kRegIcoCurrentLimit, &ico_limit), TAG,
                            "read ICO input current limit");
        state->ico_current_limit_ma = decode_input_current_limit(ico_limit);
    }
    return ESP_OK;
}

static battery_monitor_charge_phase_t decode_phase(uint8_t status1)
{
    const uint8_t phase = status1 & 0x07U;
    return phase <= BATTERY_MONITOR_PHASE_DONE
               ? (battery_monitor_charge_phase_t)phase
               : BATTERY_MONITOR_PHASE_UNKNOWN;
}

const char *battery_monitor_phase_name(battery_monitor_charge_phase_t phase)
{
    switch (phase) {
    case BATTERY_MONITOR_PHASE_NOT_CHARGING:
        return "not_charging";
    case BATTERY_MONITOR_PHASE_TRICKLE:
        return "trickle";
    case BATTERY_MONITOR_PHASE_PRECHARGE:
        return "precharge";
    case BATTERY_MONITOR_PHASE_FAST_CHARGE:
        return "fast_charge";
    case BATTERY_MONITOR_PHASE_TAPER:
        return "taper";
    case BATTERY_MONITOR_PHASE_TOP_OFF:
        return "top_off";
    case BATTERY_MONITOR_PHASE_DONE:
        return "done";
    case BATTERY_MONITOR_PHASE_UNKNOWN:
    default:
        return "unknown";
    }
}

static void state_set_detail(battery_monitor_state_t *state)
{
    if (!state->part_id_valid) {
        snprintf(state->detail, sizeof(state->detail),
                 "Unexpected SGM41529 ID: PN=%u REV=%u",
                 state->part_id, state->revision);
    } else if (!state->adc_enabled) {
        snprintf(state->detail, sizeof(state->detail),
                 "SGM41529 ADC stopped; retrying initialization");
    } else if (!state->valid) {
        snprintf(state->detail, sizeof(state->detail),
                 "Waiting for a valid battery ADC sample");
    } else if (state->fault_bits != 0) {
        snprintf(state->detail, sizeof(state->detail),
                 "SGM41529 fault 0x%02X", state->fault_bits);
    } else if (state->ntc_state == 5 || state->ntc_state == 6) {
        snprintf(state->detail, sizeof(state->detail),
                 "Battery temperature protection");
    } else if (state->low_voltage_warning) {
        snprintf(state->detail, sizeof(state->detail),
                 "Battery voltage low; please charge");
    } else if (!state->input_present) {
        snprintf(state->detail, sizeof(state->detail),
                 "Battery powered; USB input disconnected");
    } else if (state->charge_done) {
        snprintf(state->detail, sizeof(state->detail), "Charge complete");
    } else if (state->charging_current_detected) {
        snprintf(state->detail, sizeof(state->detail),
                 "Charging current detected; CHG_STAT=0");
    } else if (state->charging) {
        snprintf(state->detail, sizeof(state->detail), "Charging normally");
    } else if (state->high_impedance_mode) {
        snprintf(state->detail, sizeof(state->detail),
                 "EN_HIZ=1; charge input disabled");
    } else if (!state->register_charge_enabled) {
        snprintf(state->detail, sizeof(state->detail),
                 "EN_CHG=0; charge path disabled");
    } else if (!state->charge_pin_enabled) {
        snprintf(state->detail, sizeof(state->detail),
                 "nCE is high; XL9555 P02 disables charging");
    } else {
        snprintf(state->detail, sizeof(state->detail), "Input present; charger idle");
    }
}

static esp_err_t ensure_adc_enabled(void)
{
    uint8_t adc_control = 0;
    uint8_t adc_disable = 0;
    ESP_RETURN_ON_ERROR(read_register(kRegAdcControl, &adc_control), TAG,
                        "read ADC control");
    ESP_RETURN_ON_ERROR(read_register(kRegAdcDisable, &adc_disable), TAG,
                        "read ADC channel disable");

    const bool configuration_changed = adc_disable != 0 ||
        (adc_control & 0xF0U) != kAdcEnabledContinuous12Bit;
    if (configuration_changed) {
        ESP_LOGI(TAG, "restoring ADC: REG15=0x%02X REG16=0x%02X",
                 adc_control, adc_disable);
    }
    if (adc_disable != 0) {
        /* Enabling all ADC channels is a measurement setup operation. It does
         * not touch VREG, ICHG, EN_CHG, HIZ, or the XL9555 nCE gate. */
        ESP_RETURN_ON_ERROR(write_register(kRegAdcDisable, 0x00), TAG,
                            "enable ADC channels");
    }
    if ((adc_control & 0xF0U) != kAdcEnabledContinuous12Bit) {
        ESP_RETURN_ON_ERROR(write_register(kRegAdcControl,
                                           kAdcEnabledContinuous12Bit), TAG,
                            "enable continuous ADC");
    }
    if (configuration_changed) {
        /* Match charger_test: allow a complete conversion after enabling the
         * ADC or its channels before consuming the voltage registers. */
        vTaskDelay(pdMS_TO_TICKS(kAdcSettleMs));
    }
    s_adc_configured = true;
    return ESP_OK;
}

static esp_err_t read_sample(battery_monitor_state_t *state)
{
    uint8_t ichg_control = 0;
    uint8_t charger_control[3] = {0};
    uint8_t status[4] = {0};
    uint8_t adc[0x11] = {0};
    uint32_t io_levels = 0;

    if (!s_adc_configured) {
        ESP_RETURN_ON_ERROR(ensure_adc_enabled(), TAG, "configure ADC");
    }
    ESP_RETURN_ON_ERROR(read_register(kRegIchg, &ichg_control), TAG,
                        "read ICHG/HIZ");
    ESP_RETURN_ON_ERROR(read_registers(kRegChargerControl1, charger_control,
                                       sizeof(charger_control)), TAG,
                        "read charger controls");
    ESP_RETURN_ON_ERROR(read_registers(kRegStatus1, status, sizeof(status)), TAG,
                        "read charger status");
    ESP_RETURN_ON_ERROR(read_registers(kRegAdcControl, adc, sizeof(adc)), TAG,
                        "read charger ADC");
    ESP_RETURN_ON_ERROR(esp_io_expander_get_level(
                            s_expander,
                            BATTERY_MONITOR_XL9555_P02_CHARGE_NCE |
                                BATTERY_MONITOR_XL9555_P03_CHARGE_INT |
                                BATTERY_MONITOR_XL9555_P04_CHARGE_PGOOD,
                            &io_levels),
                        TAG, "read XL9555 charger pins");

    const uint8_t status1 = status[0];
    const uint8_t status2 = status[1];
    const uint8_t part_information = adc[kRegPartInformation - kRegAdcControl];
    const uint8_t ntc_status = status[2] & 0x07U;
    const bool adc_configuration_valid =
        (adc[0] & 0xF0U) == kAdcEnabledContinuous12Bit && adc[1] == 0;
    const int measured_ichg_ma = decode_adc_unsigned(adc[4], adc[5], 0x0F);

    memset(state, 0, sizeof(*state));
    state->battery_percent = -1;
    state->input_current_limit_ma = -1;
    state->ico_current_limit_ma = -1;
    state->phase = decode_phase(status1);
    state->device_present = true;
    state->part_id = (part_information >> 3) & 0x0FU;
    state->revision = part_information & 0x07U;
    state->part_id_valid = state->part_id == kExpectedPartId;
    state->adc_enabled = (adc[0] & 0x80U) != 0;
    state->power_good = (status2 & 0x80U) != 0;
    state->power_good_pin = (io_levels & BATTERY_MONITOR_XL9555_P04_CHARGE_PGOOD) == 0;
    state->input_present = state->power_good || state->power_good_pin;
    state->input_source = (status2 >> 4) & 0x07U;
    state->ico_status = (status2 >> 1) & 0x03U;
    state->input_limit_pin_enabled = (ichg_control & kEnIlimMask) != 0;
    state->interrupt_active = (io_levels & BATTERY_MONITOR_XL9555_P03_CHARGE_INT) == 0;
    state->input_current_regulation = (status1 & 0x40U) != 0;
    state->input_voltage_regulation = (status1 & 0x20U) != 0;
    state->thermal_regulation = (status1 & 0x10U) != 0;
    state->watchdog_expired = (status1 & 0x08U) != 0;
    state->minimum_system_regulation = (status2 & 0x01U) != 0;
    state->fault_bits = status[3] & 0xF0U;
    state->ntc_state = ntc_status;

    state->charge_pin_enabled =
        (io_levels & BATTERY_MONITOR_XL9555_P02_CHARGE_NCE) == 0;
    state->register_charge_enabled =
        (charger_control[1] & kEnChargeMask) != 0;
    state->high_impedance_mode = (ichg_control & kEnHizMask) != 0;
    state->charge_enabled = state->charge_pin_enabled &&
                            state->register_charge_enabled &&
                            !state->high_impedance_mode;
    state->ichg_ma = measured_ichg_ma;
    const bool phase_reports_charging =
        state->phase >= BATTERY_MONITOR_PHASE_TRICKLE &&
        state->phase <= BATTERY_MONITOR_PHASE_TOP_OFF;
    const bool current_reports_charging =
        state->part_id_valid && state->input_present &&
        state->charge_enabled &&
        adc_configuration_valid && state->fault_bits == 0 &&
        state->ntc_state != 5 && state->ntc_state != 6 &&
        (state->phase == BATTERY_MONITOR_PHASE_NOT_CHARGING ||
         state->phase == BATTERY_MONITOR_PHASE_UNKNOWN) &&
        measured_ichg_ma >= kChargingCurrentThresholdMa;
    state->charging_current_detected = current_reports_charging;
    /* CHG_STAT remains the primary phase report. If it says idle, the ADC
     * current fallback is accepted only when the charge gates are also on. */
    state->charge_done = state->part_id_valid && state->input_present &&
                         state->phase == BATTERY_MONITOR_PHASE_DONE;
    state->charging = state->part_id_valid && state->input_present &&
                      (phase_reports_charging || current_reports_charging);

    if (!adc_configuration_valid) {
        /* A successful I2C read does not mean the ADC configuration survived
         * a power-source change or charger reset. Retry setup next cycle just
         * as charger_test does when it detects configuration loss. */
        s_adc_configured = false;
        ESP_LOGW(TAG,
                 "ADC configuration lost: REG15=0x%02X REG16=0x%02X; reinitializing",
                 adc[0], adc[1]);
    }

    state->ibus_ma = decode_ibus(adc[2], adc[3]);
    state->vbus_mv = decode_adc_unsigned(adc[6], adc[7], 0x1F);
    state->vbat_mv = decode_adc_unsigned(adc[8], adc[9], 0x3F);
    state->vsys_mv = decode_adc_unsigned(adc[10], adc[11], 0x3F);
    const int ts_raw = decode_adc_unsigned(adc[12], adc[13], 0x03);
    const int die_raw = decode_adc_unsigned(adc[14], adc[15], 0x01);
    state->ts_deci_percent = (ts_raw * 98 + 50) / 100;
    state->die_temperature_deci_c = die_raw * 5;

    const int measured_percent =
        battery_estimator_percent_from_mv(state->vbat_mv);
    state->battery_percent = battery_estimator_limit_step(
        s_filtered_percent, measured_percent);
    if (state->battery_percent >= 0) {
        s_filtered_percent = state->battery_percent;
    } else {
        s_filtered_percent = -1;
    }

    state->valid = state->device_present && state->part_id_valid &&
                   adc_configuration_valid && state->vbat_mv > 0;
    if (!state->valid) {
        state->battery_percent = -1;
        s_filtered_percent = -1;
    }
    if (state->valid && state->input_present && state->input_source != 7) {
        const esp_err_t input_limit_ret = maintain_input_current_limit(state);
        if (input_limit_ret != ESP_OK) {
            /* A configuration failure must not discard valid voltage samples
             * or interrupt low-voltage protection. Retry on the next sample. */
            ESP_LOGW(TAG, "input current limit update failed: %s",
                     esp_err_to_name(input_limit_ret));
        }
    }
    state_set_detail(state);
    return ESP_OK;
}

static void configure_low_voltage_wakeup(void)
{
    const gpio_config_t gpio60_config = {
        .pin_bit_mask = 1ULL << BATTERY_MONITOR_GPIO60_WAKE_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t ret = gpio_config(&gpio60_config);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "GPIO60 input configuration failed: %s",
                 esp_err_to_name(ret));
        return;
    }

    /* GPIO wakeup is useful for light sleep and is harmless to configure
     * here.  On ESP32-S31 GPIO60 is a digital-only pin, so it is not an
     * EXT1/deep-sleep wake source on the current silicon. */
    ret = gpio_wakeup_enable(BATTERY_MONITOR_GPIO60_WAKE_PIN,
                             GPIO_INTR_LOW_LEVEL);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "GPIO60 light-sleep wake configuration failed: %s",
                 esp_err_to_name(ret));
    } else {
        ret = esp_sleep_enable_gpio_wakeup();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "GPIO wakeup enable failed: %s",
                     esp_err_to_name(ret));
        }
    }

    if (esp_sleep_is_valid_wakeup_gpio(BATTERY_MONITOR_GPIO60_WAKE_PIN)) {
        ret = esp_sleep_enable_ext1_wakeup_io(
            1ULL << BATTERY_MONITOR_GPIO60_WAKE_PIN,
            ESP_EXT1_WAKEUP_ANY_LOW);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "GPIO60 deep-sleep wake enable failed: %s",
                     esp_err_to_name(ret));
        }
    } else {
        ESP_LOGW(TAG,
                 "GPIO60 is not an RTC/EXT1 wake pin on this target; "
                 "deep sleep will require an external reset or a future "
                 "GPIO wake-capable silicon/IDF configuration");
    }
}

static void enter_software_sleep(bool manual, int vbat_mv)
{
    if (s_protection_sleep_started) {
        return;
    }
    s_protection_sleep_started = true;

    if (manual) {
        ESP_LOGI(TAG, "manual software shutdown requested; entering deep sleep");
    } else {
        ESP_LOGE(TAG,
                 "latched low-voltage deadline reached; trigger=%dmV "
                 "last_valid=%dmV; entering deep sleep",
                 s_shutdown_trigger_voltage_mv, vbat_mv);
    }
    /* Neither shutdown source may wait for an LVGL/emote notification. */
    (void)esp_xiaozhi_chat_display_set_brightness(0);

    /* Remove avoidable loads before the PMU cuts the running system down. */
    (void)tbao_s31_board_set_speaker(false);
    (void)tbao_s31_board_set_sd_power(false);
    (void)tbao_s31_board_set_5v_power(false);
    configure_low_voltage_wakeup();

    vTaskDelay(pdMS_TO_TICKS(350));
    esp_deep_sleep_start();
}

static void cancel_shutdown_countdown(void)
{
    if (!s_shutdown_countdown_active) {
        return;
    }
    s_shutdown_countdown_active = false;
    s_shutdown_countdown_deadline = 0;
    s_shutdown_countdown_last_second = -1;
    s_shutdown_voltage_samples = 0;
    s_charging_confirm_samples = 0;
    ESP_LOGI(TAG,
             "shutdown countdown cancelled: charging_confirmed (%u fresh samples)",
             (unsigned)BATTERY_MONITOR_CHARGING_CONFIRM_SAMPLES);
}

/* Called by the protection task even when ADC/I2C or the UI is unavailable. */
static bool update_shutdown_countdown(void)
{
    if (!s_shutdown_countdown_active) {
        return false;
    }
    const TickType_t now = xTaskGetTickCount();
    const TickType_t one_second_ticks = pdMS_TO_TICKS(1000);
    int remaining_seconds = 0;
    if ((int32_t)(s_shutdown_countdown_deadline - now) > 0) {
        const TickType_t remaining_ticks = s_shutdown_countdown_deadline - now;
        const TickType_t tick_unit = one_second_ticks > 0 ? one_second_ticks : 1;
        remaining_seconds = (int)((remaining_ticks + tick_unit - 1) / tick_unit);
        if (remaining_seconds > kShutdownCountdownSeconds) {
            remaining_seconds = kShutdownCountdownSeconds;
        }
    }
    if (remaining_seconds != s_shutdown_countdown_last_second) {
        s_shutdown_countdown_last_second = remaining_seconds;
        ESP_LOGW(TAG, "battery shutdown countdown: %d seconds remaining",
                 remaining_seconds);
    }
    return remaining_seconds == 0;
}

/* Only fresh hardware samples enter this function, never cached UI values. */
static void update_low_voltage_protection(const battery_monitor_state_t *state)
{
    if (state == NULL || !state->valid || state->data_stale || state->vbat_mv <= 0) {
        s_low_voltage_samples = 0;
        s_shutdown_voltage_samples = 0;
        s_charging_confirm_samples = 0;
        if (s_shutdown_countdown_active) {
            ESP_LOGW(TAG, "ADC/I2C sample unavailable; latched shutdown countdown retained");
        }
        return;
    }

    if (state->vbat_mv < BATTERY_MONITOR_LOW_VOLTAGE_MV) {
        if (s_low_voltage_samples < BATTERY_MONITOR_LOW_VOLTAGE_SAMPLES) {
            ++s_low_voltage_samples;
        }
    } else if (state->vbat_mv >= BATTERY_MONITOR_LOW_VOLTAGE_CLEAR_MV) {
        s_low_voltage_samples = 0;
        s_low_voltage_warning = false;
    }
    if (s_low_voltage_samples >= BATTERY_MONITOR_LOW_VOLTAGE_SAMPLES) {
        s_low_voltage_warning = true;
    }

    const bool actively_charging =
        state->input_present && state->charge_enabled &&
        state->fault_bits == 0 && state->ntc_state != 5 && state->ntc_state != 6 &&
        (state->charging || state->charging_current_detected);

    if (s_shutdown_countdown_active) {
        if (actively_charging) {
            if (s_charging_confirm_samples < BATTERY_MONITOR_CHARGING_CONFIRM_SAMPLES) {
                ++s_charging_confirm_samples;
            }
            ESP_LOGI(TAG, "shutdown charging confirmation: %u/%u",
                     s_charging_confirm_samples,
                     (unsigned)BATTERY_MONITOR_CHARGING_CONFIRM_SAMPLES);
            if (s_charging_confirm_samples >= BATTERY_MONITOR_CHARGING_CONFIRM_SAMPLES) {
                cancel_shutdown_countdown();
            }
        } else {
            s_charging_confirm_samples = 0;
        }
        /* Voltage rebound is not a cancellation condition after latching. */
        return;
    }

    if (state->vbat_mv < BATTERY_MONITOR_SHUTDOWN_VOLTAGE_MV &&
        !actively_charging) {
        if (s_shutdown_voltage_samples < BATTERY_MONITOR_SHUTDOWN_SAMPLES) {
            ++s_shutdown_voltage_samples;
        }
    } else {
        s_shutdown_voltage_samples = 0;
    }

    if (s_shutdown_voltage_samples >= BATTERY_MONITOR_SHUTDOWN_SAMPLES) {
        s_shutdown_countdown_active = true;
        s_shutdown_countdown_deadline = xTaskGetTickCount() +
            pdMS_TO_TICKS(kShutdownCountdownSeconds * 1000);
        s_shutdown_countdown_last_second = -1;
        s_shutdown_trigger_voltage_mv = state->vbat_mv;
        s_charging_confirm_samples = 0;
        ESP_LOGW(TAG,
                 "battery voltage %dmV below %dmV for %u samples; "
                 "latched shutdown countdown started (%ds)",
                 state->vbat_mv, BATTERY_MONITOR_SHUTDOWN_VOLTAGE_MV,
                 (unsigned)BATTERY_MONITOR_SHUTDOWN_SAMPLES,
                 kShutdownCountdownSeconds);
    }
}

static void publish_state(const battery_monitor_state_t *state)
{
    if (s_state_mutex == NULL) {
        return;
    }
    if (xSemaphoreTake(s_state_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    s_state = *state;
    xSemaphoreGive(s_state_mutex);

    if (s_display_queue != NULL) {
        /* A stalled renderer must never block sampling or protection. */
        (void)xQueueOverwrite(s_display_queue, state);
    }
}

static void make_display_state(const battery_sample_t *sample,
                               const battery_monitor_state_t *last_valid,
                               bool have_last_valid,
                               TickType_t last_valid_at,
                               battery_monitor_state_t *out)
{
    const TickType_t now = xTaskGetTickCount();
    const TickType_t hold_ticks = pdMS_TO_TICKS(BATTERY_MONITOR_DISPLAY_HOLD_MS);
    const bool fresh = sample->error == ESP_OK && sample->state.valid &&
                       (now - sample->sampled_at) <= hold_ticks;
    const TickType_t age = have_last_valid ? now - last_valid_at : 0;

    *out = sample->state;
    out->valid = fresh;
    out->data_stale = false;
    out->sample_age_ms = have_last_valid
        ? (uint32_t)((uint64_t)age * 1000U / configTICK_RATE_HZ) : 0;
    if (!fresh && have_last_valid && age <= hold_ticks) {
        /* UI-only grace period: the protection task has already processed the
         * actual failed/invalid sample, never this cached presentation state. */
        *out = *last_valid;
        out->valid = false;
        out->data_stale = true;
        out->sample_age_ms =
            (uint32_t)((uint64_t)age * 1000U / configTICK_RATE_HZ);
        snprintf(out->detail, sizeof(out->detail),
                 "Data temporarily unavailable; last valid sample %ums ago",
                 (unsigned)out->sample_age_ms);
    } else if (!fresh && sample->state.valid) {
        out->battery_percent = -1;
        snprintf(out->detail, sizeof(out->detail),
                 "Battery monitor sample timed out");
    }

    out->low_voltage_warning = s_low_voltage_warning || s_shutdown_countdown_active;
    out->shutdown_countdown_active = s_shutdown_countdown_active;
    out->shutdown_seconds_remaining = s_shutdown_countdown_active
        ? s_shutdown_countdown_last_second : -1;
    if (out->valid) {
        state_set_detail(out);
    }
}

static void battery_protection_task(void *arg)
{
    (void)arg;
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    battery_sample_t latest_sample = {0};
    battery_monitor_state_t last_valid = {0};
    battery_monitor_state_t last_published = {0};
    bool have_sample = false;
    bool have_last_valid = false;
    bool have_published = false;
    TickType_t last_valid_at = 0;

    for (;;) {
        portENTER_CRITICAL(&s_sleep_request_lock);
        const bool manual_sleep_requested = s_manual_sleep_requested;
        portEXIT_CRITICAL(&s_sleep_request_lock);
        if (manual_sleep_requested) {
            enter_software_sleep(true, have_last_valid ? last_valid.vbat_mv : 0);
        }
        battery_sample_t incoming;
        const bool received = xQueueReceive(s_sample_queue, &incoming,
            pdMS_TO_TICKS(kProtectionPollMs)) == pdTRUE;
        if (received) {
            if (have_sample &&
                (incoming.sampled_at - latest_sample.sampled_at) >
                    pdMS_TO_TICKS(BATTERY_MONITOR_DISPLAY_HOLD_MS)) {
                /* Samples separated by a long gap are not consecutive. This
                 * only resets confirmation counters, never a latched deadline. */
                s_low_voltage_samples = 0;
                s_shutdown_voltage_samples = 0;
                s_charging_confirm_samples = 0;
            }
            latest_sample = incoming;
            have_sample = true;
            const bool fresh = incoming.error == ESP_OK && incoming.state.valid &&
                (xTaskGetTickCount() - incoming.sampled_at) <=
                    pdMS_TO_TICKS(BATTERY_MONITOR_DISPLAY_HOLD_MS);
            update_low_voltage_protection(fresh ? &incoming.state : NULL);
            if (fresh) {
                last_valid = incoming.state;
                last_valid_at = incoming.sampled_at;
                have_last_valid = true;
            }
        } else if (have_sample &&
                   (xTaskGetTickCount() - latest_sample.sampled_at) >
                       pdMS_TO_TICKS(BATTERY_MONITOR_DISPLAY_HOLD_MS)) {
            /* A stalled sampler breaks all pre-trigger/charging confirmation
             * sequences, while the independent shutdown deadline keeps running. */
            s_low_voltage_samples = 0;
            s_shutdown_voltage_samples = 0;
            s_charging_confirm_samples = 0;
        }

        const bool shutdown_due = update_shutdown_countdown();
        if (have_sample) {
            battery_monitor_state_t display_state;
            make_display_state(&latest_sample, &last_valid, have_last_valid,
                               last_valid_at, &display_state);
            if (received || !have_published ||
                display_state.valid != last_published.valid ||
                display_state.data_stale != last_published.data_stale ||
                display_state.shutdown_countdown_active !=
                    last_published.shutdown_countdown_active ||
                display_state.shutdown_seconds_remaining !=
                    last_published.shutdown_seconds_remaining) {
                publish_state(&display_state);
                last_published = display_state;
                have_published = true;
            }
        }
        if (shutdown_due) {
            /* No I2C sampling or LVGL calls stand between the deadline and
             * the shutdown path. Missing ADC data is not voltage recovery. */
            enter_software_sleep(false, have_last_valid ? last_valid.vbat_mv
                                                       : s_shutdown_trigger_voltage_mv);
        }
    }
}

static void battery_display_task(void *arg)
{
    (void)arg;
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    bool countdown_was_active = false;
    bool low_voltage_notified = false;

    for (;;) {
        battery_monitor_state_t state;
        if (xQueueReceive(s_display_queue, &state, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (s_callback != NULL) {
            s_callback(&state, s_callback_context);
        }
        /* The callback may have waited for the renderer. Discard its old
         * countdown snapshot and render the newest backend state instead. */
        if (battery_monitor_get_state(&state) != ESP_OK) {
            continue;
        }
        if (state.shutdown_countdown_active) {
            countdown_was_active = true;
            (void)esp_xiaozhi_chat_display_show_shutdown_countdown(
                state.shutdown_seconds_remaining);
        } else if (countdown_was_active &&
                   esp_xiaozhi_chat_display_hide_shutdown_countdown() == ESP_OK) {
            countdown_was_active = false;
        }

        if (!state.low_voltage_warning) {
            low_voltage_notified = false;
        } else if (!low_voltage_notified && !state.shutdown_countdown_active) {
            low_voltage_notified = esp_xiaozhi_chat_display_set_notification(
                app_language_text("电量低于7V，请及时充电",
                                  "Battery below 7V. Please charge."),
                3000) == ESP_OK;
        }
    }
}

static void submit_invalid_sample(esp_err_t error, int failures)
{
    battery_monitor_state_t state = {0};
    state.battery_percent = -1;
    state.input_current_limit_ma = -1;
    state.ico_current_limit_ma = -1;
    state.shutdown_seconds_remaining = -1;
    state.phase = BATTERY_MONITOR_PHASE_UNKNOWN;
    snprintf(state.detail, sizeof(state.detail),
             "SGM41529 read failed (%d/%d): %s",
             failures, kReadFailureLimit, esp_err_to_name(error));
    const battery_sample_t sample = {
        .state = state,
        .error = error,
        .sampled_at = xTaskGetTickCount(),
    };
    (void)xQueueOverwrite(s_sample_queue, &sample);
}

static void battery_monitor_task(void *arg)
{
    (void)arg;
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    int read_failures = 0;
    battery_monitor_state_t last_sample = {0};
    bool have_last_sample = false;
    unsigned sample_count = 0;

    while (true) {
        battery_monitor_state_t state;
        const esp_err_t ret = read_sample(&state);
        if (ret != ESP_OK) {
            ++read_failures;
            if (read_failures > kReadFailureLimit) {
                read_failures = kReadFailureLimit;
            }
            s_adc_configured = false;
            have_last_sample = false;
            submit_invalid_sample(ret, read_failures);
            ESP_LOGW(TAG, "battery sample failed (%d/%d): %s",
                     read_failures, kReadFailureLimit, esp_err_to_name(ret));
        } else {
            read_failures = 0;
            const battery_sample_t sample = {
                .state = state,
                .error = ESP_OK,
                .sampled_at = xTaskGetTickCount(),
            };
            (void)xQueueOverwrite(s_sample_queue, &sample);
            ESP_LOGI(TAG, "USB输入：%dmA 电池充电：%dmA",
                     state.ibus_ma, state.ichg_ma);
            const bool state_changed = !have_last_sample ||
                state.valid != last_sample.valid ||
                state.input_present != last_sample.input_present ||
                state.input_source != last_sample.input_source ||
                state.input_current_limit_ma != last_sample.input_current_limit_ma ||
                state.ico_status != last_sample.ico_status ||
                state.ico_current_limit_ma != last_sample.ico_current_limit_ma ||
                state.input_limit_pin_enabled != last_sample.input_limit_pin_enabled ||
                state.phase != last_sample.phase ||
                state.charging != last_sample.charging ||
                state.charging_current_detected != last_sample.charging_current_detected ||
                state.low_voltage_warning != last_sample.low_voltage_warning ||
                state.charge_pin_enabled != last_sample.charge_pin_enabled ||
                state.register_charge_enabled != last_sample.register_charge_enabled ||
                state.high_impedance_mode != last_sample.high_impedance_mode ||
                state.adc_enabled != last_sample.adc_enabled ||
                state.watchdog_expired != last_sample.watchdog_expired ||
                state.fault_bits != last_sample.fault_bits ||
                state.ntc_state != last_sample.ntc_state;
            if (state_changed || (sample_count % 10U) == 0U) {
                ESP_LOGI(TAG,
                         "valid=%d USB=%d PG=%d/%d phase=%s charging=%d "
                         "VBUS=%dmV VBAT=%dmV ICHG=%dmA percent=%d "
                         "current_detected=%d nCE=%s EN_CHG=%d HIZ=%d "
                         "ADC=%d WD=%d NTC=%u faults=0x%02X "
                         "VBUS_STAT=%u IINDPM=%dmA ICO_STAT=%u ICO_ILIM=%dmA "
                         "EN_ILIM=%d IINDPM_STAT=%d VINDPM_STAT=%d TREG_STAT=%d",
                         state.valid, state.input_present,
                         state.power_good, state.power_good_pin,
                         battery_monitor_phase_name(state.phase), state.charging,
                         state.vbus_mv, state.vbat_mv, state.ichg_ma,
                         state.battery_percent,
                         state.charging_current_detected,
                         state.charge_pin_enabled ? "LOW" : "HIGH",
                         state.register_charge_enabled, state.high_impedance_mode,
                         state.adc_enabled, state.watchdog_expired,
                         state.ntc_state, state.fault_bits,
                         (unsigned)state.input_source, state.input_current_limit_ma,
                         (unsigned)state.ico_status, state.ico_current_limit_ma,
                         state.input_limit_pin_enabled, state.input_current_regulation,
                         state.input_voltage_regulation, state.thermal_regulation);
            }
            last_sample = state;
            have_last_sample = true;
            ++sample_count;
        }
        vTaskDelay(pdMS_TO_TICKS(BATTERY_MONITOR_SAMPLE_PERIOD_MS));
    }
}

esp_err_t battery_monitor_get_state(battery_monitor_state_t *out_state)
{
    if (out_state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_state_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_state_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    *out_state = s_state;
    xSemaphoreGive(s_state_mutex);
    return ESP_OK;
}

esp_err_t battery_monitor_request_sleep(void)
{
    if (!s_started || s_protection_task == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&s_sleep_request_lock);
    s_manual_sleep_requested = true;
    portEXIT_CRITICAL(&s_sleep_request_lock);
    return ESP_OK;
}

esp_err_t battery_monitor_start(battery_monitor_callback_t callback,
                                void *context)
{
    if (s_started) {
        return ESP_ERR_INVALID_STATE;
    }

    i2c_master_bus_handle_t bus = NULL;
    esp_err_t ret = tbao_s31_board_get_i2c_bus(&bus);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = tbao_s31_board_get_io_expander(&s_expander);
    if (ret != ESP_OK || s_expander == NULL) {
        s_expander = NULL;
        return ret == ESP_OK ? ESP_ERR_INVALID_STATE : ret;
    }

    /* P03/P04 are status inputs. P02 is intentionally left in the direction
     * selected by the board profile: changing its direction without first
     * knowing the XL9555 output latch could accidentally assert nCE and start
     * charging. */
    ret = esp_io_expander_set_dir(
        s_expander,
        BATTERY_MONITOR_XL9555_P03_CHARGE_INT |
            BATTERY_MONITOR_XL9555_P04_CHARGE_PGOOD,
        IO_EXPANDER_INPUT);
    if (ret != ESP_OK) {
        s_expander = NULL;
        return ret;
    }

    i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BATTERY_MONITOR_SGM41529_ADDRESS,
        .scl_speed_hz = 100000,
    };
    ret = i2c_master_bus_add_device(bus, &config, &s_charger);
    if (ret != ESP_OK) {
        s_charger = NULL;
        s_expander = NULL;
        return ret;
    }

    s_state_mutex = xSemaphoreCreateMutex();
    if (s_state_mutex == NULL) {
        (void)i2c_master_bus_rm_device(s_charger);
        s_charger = NULL;
        s_expander = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_callback = callback;
    s_callback_context = context;
    s_adc_configured = false;
    s_filtered_percent = -1;
    s_low_voltage_samples = 0;
    s_shutdown_voltage_samples = 0;
    s_low_voltage_warning = false;
    s_charging_confirm_samples = 0;
    s_shutdown_countdown_active = false;
    s_shutdown_countdown_deadline = 0;
    s_shutdown_countdown_last_second = -1;
    s_shutdown_trigger_voltage_mv = 0;
    s_protection_sleep_started = false;
    s_manual_sleep_requested = false;
    memset(&s_state, 0, sizeof(s_state));
    s_state.battery_percent = -1;
    s_state.input_current_limit_ma = -1;
    s_state.ico_current_limit_ma = -1;
    s_state.shutdown_seconds_remaining = -1;
    s_state.phase = BATTERY_MONITOR_PHASE_UNKNOWN;
    snprintf(s_state.detail, sizeof(s_state.detail),
             "Initializing SGM41529 at 0x%02X",
             BATTERY_MONITOR_SGM41529_ADDRESS);

    /* Single-slot mailboxes always keep the newest sample/state. Neither
     * acquisition nor the deadline supervisor waits for the display worker. */
    s_sample_queue = xQueueCreate(1, sizeof(battery_sample_t));
    s_display_queue = xQueueCreate(1, sizeof(battery_monitor_state_t));
    if (s_sample_queue == NULL || s_display_queue == NULL) {
        goto start_failed;
    }
    publish_state(&s_state);
    /* This worker only updates LVGL/emote state and timers. It never performs
     * I2C, NVS, or flash operations, so its stack can live in PSRAM. Keep an
     * internal fallback so a PSRAM allocation failure cannot disable battery
     * sampling and low-voltage protection. */
    BaseType_t display_task_ret = xTaskCreatePinnedToCoreWithCaps(
        battery_display_task, "battery_ui", 4096, NULL, 4, &s_display_task,
        tskNO_AFFINITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (display_task_ret != pdPASS) {
        ESP_LOGW(TAG, "battery UI PSRAM stack unavailable; retrying internal");
        display_task_ret = xTaskCreatePinnedToCoreWithCaps(
            battery_display_task, "battery_ui", 4096, NULL, 4,
            &s_display_task, tskNO_AFFINITY,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (display_task_ret != pdPASS) {
        goto start_failed;
    }
    if (xTaskCreate(battery_protection_task, "battery_protect", 4096, NULL, 6,
                    &s_protection_task) != pdPASS) {
        goto start_failed;
    }
    if (xTaskCreate(battery_monitor_task, "battery_monitor", 4096, NULL, 5,
                    &s_task) != pdPASS) {
        goto start_failed;
    }

    s_started = true;
    /* Workers cannot run callbacks or touch shared resources until all three
     * tasks were created successfully, making failure cleanup safe as well. */
    xTaskNotifyGive(s_display_task);
    xTaskNotifyGive(s_protection_task);
    xTaskNotifyGive(s_task);
    ESP_LOGI(TAG, "monitor started: SGM41529=0x%02X XL9555=0x%02X sample=%dms",
             BATTERY_MONITOR_SGM41529_ADDRESS,
             BATTERY_MONITOR_XL9555_ADDRESS,
             BATTERY_MONITOR_SAMPLE_PERIOD_MS);
    ESP_LOGI(TAG,
             "latched shutdown: %dmV/%u samples, %ds deadline; "
             "charging confirmation=%u samples; UI hold=%dms",
             BATTERY_MONITOR_SHUTDOWN_VOLTAGE_MV,
             (unsigned)BATTERY_MONITOR_SHUTDOWN_SAMPLES,
             kShutdownCountdownSeconds,
             (unsigned)BATTERY_MONITOR_CHARGING_CONFIRM_SAMPLES,
             BATTERY_MONITOR_DISPLAY_HOLD_MS);
    return ESP_OK;

start_failed:
    if (s_task != NULL) {
        vTaskDelete(s_task);
        s_task = NULL;
    }
    if (s_protection_task != NULL) {
        vTaskDelete(s_protection_task);
        s_protection_task = NULL;
    }
    if (s_display_task != NULL) {
        vTaskDeleteWithCaps(s_display_task);
        s_display_task = NULL;
    }
    if (s_sample_queue != NULL) {
        vQueueDelete(s_sample_queue);
        s_sample_queue = NULL;
    }
    if (s_display_queue != NULL) {
        vQueueDelete(s_display_queue);
        s_display_queue = NULL;
    }
    vSemaphoreDelete(s_state_mutex);
    s_state_mutex = NULL;
    (void)i2c_master_bus_rm_device(s_charger);
    s_charger = NULL;
    s_expander = NULL;
    return ESP_ERR_NO_MEM;
}
