#pragma once

#include "esp_err.h"

constexpr int kMotorPercentMin = -100;
constexpr int kMotorPercentMax = 100;
constexpr int kServoPulseMinUs = 1100;
constexpr int kServoPulseCenterUs = 1500;
constexpr int kServoPulseMaxUs = 1900;

struct ActuatorState {
    int motor_a_percent = 0;
    int motor_b_percent = 0;
    int servo_pulse_us = kServoPulseCenterUs;
    bool servo_enabled = false;
    bool failsafe_active = false;
};

using ActuatorStateCallback =
    void (*)(const ActuatorState &state, void *context);

esp_err_t actuator_control_init(
    ActuatorStateCallback callback,
    void *context);
esp_err_t actuator_control_apply(
    int motor_a_percent,
    int motor_b_percent,
    int servo_pulse_us,
    bool servo_enabled);
void actuator_control_stop_all(bool failsafe_active = false);
ActuatorState actuator_control_get_state();
