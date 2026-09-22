#pragma once

#include "actuator_control.h"
#include "esp_err.h"
#include "wifi_manager.h"

esp_err_t display_ui_init();
void display_ui_update_wifi(const WifiStatus &status);
void display_ui_update_actuators(const ActuatorState &state);
