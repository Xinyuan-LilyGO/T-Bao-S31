#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ACTUATOR_MOTOR_PERCENT_MIN (-100)
#define ACTUATOR_MOTOR_PERCENT_MAX 100
#define ACTUATOR_SERVO_PULSE_MIN_US 1100
#define ACTUATOR_SERVO_PULSE_CENTER_US 1500
#define ACTUATOR_SERVO_PULSE_MAX_US 1900

typedef struct {
    int motor_a_percent;
    int motor_b_percent;
    int servo_pulse_us;
    bool servo_enabled;
    bool failsafe_active;
} actuator_state_t;

typedef void (*actuator_state_callback_t)(const actuator_state_t *state,
                                          void *context);

esp_err_t actuator_control_init(actuator_state_callback_t callback,
                                void *context);
esp_err_t actuator_control_apply(int motor_a_percent,
                                 int motor_b_percent,
                                 int servo_pulse_us,
                                 bool servo_enabled);
void actuator_control_stop_all(bool failsafe_active);
esp_err_t actuator_control_get_state(actuator_state_t *out_state);

#ifdef __cplusplus
}
#endif
