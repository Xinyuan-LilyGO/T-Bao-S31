#pragma once

#include "charger_test.h"
#include "esp_err.h"

esp_err_t display_ui_init();
void display_ui_update(const ChargerTestState &state);
