#pragma once

#include "audio_test.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_io_expander.h"

struct DisplayUiHandles {
    i2c_master_bus_handle_t i2c_bus = nullptr;
    esp_io_expander_handle_t io_expander = nullptr;
};

using UiActionCallback = void (*)(TestMode mode, void *context);

esp_err_t display_ui_init(
    UiActionCallback callback,
    void *callback_context,
    DisplayUiHandles *handles_out);

void display_ui_update(const AudioTestState &state);
