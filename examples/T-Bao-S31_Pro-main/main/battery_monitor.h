/* T-Bao S31 SGM41529 charger and battery monitor. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BATTERY_MONITOR_SGM41529_ADDRESS       0x6B
#define BATTERY_MONITOR_XL9555_ADDRESS         0x22
#define BATTERY_MONITOR_XL9555_P02_CHARGE_NCE  (1U << 2)
#define BATTERY_MONITOR_XL9555_P03_CHARGE_INT  (1U << 3)
#define BATTERY_MONITOR_XL9555_P04_CHARGE_PGOOD (1U << 4)
#define BATTERY_MONITOR_SAMPLE_PERIOD_MS       1000

/* Host input-current ceiling; ILIM, ICO, voltage and thermal limits stay active. */
#define BATTERY_MONITOR_INPUT_CURRENT_LIMIT_MA 3300

/* Battery protection thresholds.  The monitor uses the charger ADC voltage
 * directly, so keep these values in millivolts. */
#define BATTERY_MONITOR_LOW_VOLTAGE_MV         7000
#define BATTERY_MONITOR_SHUTDOWN_VOLTAGE_MV    6500
#define BATTERY_MONITOR_LOW_VOLTAGE_CLEAR_MV   7300
#define BATTERY_MONITOR_LOW_VOLTAGE_SAMPLES    15
#define BATTERY_MONITOR_SHUTDOWN_SAMPLES       15
#define BATTERY_MONITOR_CHARGING_CONFIRM_SAMPLES 3
#define BATTERY_MONITOR_DISPLAY_HOLD_MS        3000
#define BATTERY_MONITOR_GPIO60_WAKE_PIN        GPIO_NUM_60

typedef enum {
    BATTERY_MONITOR_PHASE_NOT_CHARGING = 0,
    BATTERY_MONITOR_PHASE_TRICKLE = 1,
    BATTERY_MONITOR_PHASE_PRECHARGE = 2,
    BATTERY_MONITOR_PHASE_FAST_CHARGE = 3,
    BATTERY_MONITOR_PHASE_TAPER = 4,
    BATTERY_MONITOR_PHASE_TOP_OFF = 5,
    BATTERY_MONITOR_PHASE_DONE = 6,
    BATTERY_MONITOR_PHASE_UNKNOWN = 7,
} battery_monitor_charge_phase_t;

typedef struct {
    /* valid is reserved for fresh ADC data. Cached UI readings are explicitly
     * marked data_stale and must never be used for protection decisions. */
    bool valid;
    bool data_stale;
    bool device_present;
    bool part_id_valid;
    bool adc_enabled;
    bool power_good;
    bool power_good_pin;
    bool input_present;
    bool interrupt_active;
    bool charge_pin_enabled;
    bool register_charge_enabled;
    bool high_impedance_mode;
    bool charge_enabled;
    bool charging;
    /* CHRG_STAT can briefly stay at 000 while the battery ADC already shows
     * charge current. This flag records that secondary evidence explicitly. */
    bool charging_current_detected;
    bool charge_done;
    bool low_voltage_warning;
    bool shutdown_countdown_active;
    bool input_limit_pin_enabled;
    bool input_current_regulation;
    bool input_voltage_regulation;
    bool thermal_regulation;
    bool watchdog_expired;
    bool minimum_system_regulation;

    uint8_t part_id;
    uint8_t revision;
    uint8_t fault_bits;
    uint8_t ntc_state;
    uint8_t input_source; /* VBUS_STAT[2:0] from REG0C[6:4]. */
    uint8_t ico_status;
    battery_monitor_charge_phase_t phase;

    int battery_percent;
    int vbus_mv;
    int vbat_mv;
    int vsys_mv;
    int ibus_ma;
    int ichg_ma;
    int input_current_limit_ma; /* REG03 IINDPM; -1 when unavailable. */
    int ico_current_limit_ma; /* REG0A limit in ICO mode; otherwise -1. */
    int shutdown_seconds_remaining; /* -1 when no shutdown is pending. */
    uint32_t sample_age_ms;
    int die_temperature_deci_c;
    int ts_deci_percent;

    char detail[96];
} battery_monitor_state_t;

typedef void (*battery_monitor_callback_t)(
    const battery_monitor_state_t *state,
    void *context);

/** Start the periodic monitor on the Board Manager primary I2C bus. */
esp_err_t battery_monitor_start(battery_monitor_callback_t callback,
                                void *context);

/** Return the most recently sampled state. */
esp_err_t battery_monitor_get_state(battery_monitor_state_t *out_state);

/** Request manual software shutdown (deep sleep) from the protection worker.
 * Returns immediately; no low-voltage countdown or charging cancellation is
 * applied. Wake sources follow the existing board configuration. The monitor
 * must have started successfully; repeated requests are harmless. */
esp_err_t battery_monitor_request_sleep(void);

/** Return a stable human-readable name for a charge phase. */
const char *battery_monitor_phase_name(battery_monitor_charge_phase_t phase);

#ifdef __cplusplus
}
#endif
