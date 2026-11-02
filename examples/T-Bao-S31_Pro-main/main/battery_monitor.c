#include "battery_monitor.h"

#include "battery_estimator.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_io_expander.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
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
    /* SGM41529 ICHG ADC is 1mA/LSB. Keep the fallback above idle noise while
     * still detecting the 50-80mA trickle/pre-charge ranges. */
    kChargingCurrentThresholdMa = 30,
    kRegIchg = 0x01,
    kRegChargerControl1 = 0x05,
    kRegStatus1 = 0x0B,
    kRegStatus2 = 0x0C,
    kRegNtcStatus = 0x0D,
    kRegFaultStatus = 0x0E,
    kRegAdcControl = 0x15,
    kRegAdcDisable = 0x16,
    kRegPartInformation = 0x25,
};

/* SGM41529 status/control bits. The monitor never writes the charge-control
 * registers; these masks only decode the state established by the board or
 * the charger itself. */
static const uint8_t kEnHizMask = 1U << 7;
static const uint8_t kEnChargeMask = 1U << 3;

static i2c_master_dev_handle_t s_charger;
static esp_io_expander_handle_t s_expander;
static SemaphoreHandle_t s_state_mutex;
static TaskHandle_t s_task;
static battery_monitor_callback_t s_callback;
static void *s_callback_context;
static bool s_started;
static bool s_adc_configured;
static int s_filtered_percent = -1;
static battery_monitor_state_t s_state = {
    .battery_percent = -1,
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
    state->phase = decode_phase(status1);
    state->device_present = true;
    state->part_id = (part_information >> 3) & 0x0FU;
    state->revision = part_information & 0x07U;
    state->part_id_valid = state->part_id == kExpectedPartId;
    state->adc_enabled = (adc[0] & 0x80U) != 0;
    state->power_good = (status2 & 0x80U) != 0;
    state->power_good_pin = (io_levels & BATTERY_MONITOR_XL9555_P04_CHARGE_PGOOD) == 0;
    state->input_present = state->power_good || state->power_good_pin;
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
    state_set_detail(state);
    return ESP_OK;
}

static void publish_state(const battery_monitor_state_t *state)
{
    battery_monitor_callback_t callback;
    void *context;
    if (s_state_mutex == NULL) {
        return;
    }
    if (xSemaphoreTake(s_state_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    s_state = *state;
    callback = s_callback;
    context = s_callback_context;
    xSemaphoreGive(s_state_mutex);

    if (callback != NULL) {
        callback(state, context);
    }
}

static void publish_invalid_state(esp_err_t error, int failures)
{
    battery_monitor_state_t state = {0};
    state.battery_percent = -1;
    state.phase = BATTERY_MONITOR_PHASE_UNKNOWN;
    snprintf(state.detail, sizeof(state.detail),
             "SGM41529 read failed (%d/%d): %s",
             failures, kReadFailureLimit, esp_err_to_name(error));
    publish_state(&state);
}

static void battery_monitor_task(void *arg)
{
    (void)arg;
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
            s_filtered_percent = -1;
            have_last_sample = false;
            publish_invalid_state(ret, read_failures);
            ESP_LOGW(TAG, "battery sample failed (%d/%d): %s",
                     read_failures, kReadFailureLimit, esp_err_to_name(ret));
        } else {
            read_failures = 0;
            publish_state(&state);
            const bool state_changed = !have_last_sample ||
                state.valid != last_sample.valid ||
                state.input_present != last_sample.input_present ||
                state.phase != last_sample.phase ||
                state.charging != last_sample.charging ||
                state.charging_current_detected != last_sample.charging_current_detected ||
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
                         "ADC=%d WD=%d NTC=%u faults=0x%02X",
                         state.valid, state.input_present,
                         state.power_good, state.power_good_pin,
                         battery_monitor_phase_name(state.phase), state.charging,
                         state.vbus_mv, state.vbat_mv, state.ichg_ma,
                         state.battery_percent,
                         state.charging_current_detected,
                         state.charge_pin_enabled ? "LOW" : "HIGH",
                         state.register_charge_enabled, state.high_impedance_mode,
                         state.adc_enabled, state.watchdog_expired,
                         state.ntc_state, state.fault_bits);
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
    memset(&s_state, 0, sizeof(s_state));
    s_state.battery_percent = -1;
    s_state.phase = BATTERY_MONITOR_PHASE_UNKNOWN;
    snprintf(s_state.detail, sizeof(s_state.detail),
             "Initializing SGM41529 at 0x%02X",
             BATTERY_MONITOR_SGM41529_ADDRESS);

    if (xTaskCreate(battery_monitor_task, "battery_monitor", 4096, NULL, 5,
                    &s_task) != pdPASS) {
        vSemaphoreDelete(s_state_mutex);
        s_state_mutex = NULL;
        (void)i2c_master_bus_rm_device(s_charger);
        s_charger = NULL;
        s_expander = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_started = true;
    publish_state(&s_state);
    ESP_LOGI(TAG, "monitor started: SGM41529=0x%02X XL9555=0x%02X sample=%dms",
             BATTERY_MONITOR_SGM41529_ADDRESS,
             BATTERY_MONITOR_XL9555_ADDRESS,
             BATTERY_MONITOR_SAMPLE_PERIOD_MS);
    return ESP_OK;
}
