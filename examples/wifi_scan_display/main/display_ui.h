#pragma once

#include "esp_err.h"
#include "wifi_test.h"

esp_err_t display_ui_init();
void display_ui_update(const WifiTestState &state);
