#pragma once

#include <cstdint>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_io_expander.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

#include "factory_core.h"

struct FactoryBoardState {
    i2c_master_bus_handle_t i2c_bus = nullptr;
    esp_io_expander_handle_t io_expander = nullptr;
    esp_lcd_panel_io_handle_t lcd_io = nullptr;
    esp_lcd_panel_handle_t lcd_panel = nullptr;
    bool i2c_ready = false;
    bool expander_ready = false;
    bool display_ready = false;
    bool touch_ready = false;
};

using FactoryUiActionCallback = void (*)(const FactoryAction &action, void *context);

esp_err_t board_service_init(FactoryUiActionCallback callback, void *context);
const FactoryBoardState &board_service_state();
esp_err_t board_service_enable_backlight();
esp_err_t board_service_set_output(uint32_t mask, bool high);
esp_err_t board_service_get_level(uint32_t mask, uint32_t *levels);
esp_err_t board_service_apply_safe_outputs();
uint32_t board_service_touch_error_count();
void board_service_reset_touch_error_count();

void board_ui_show_boot(
    const char *firmware_version,
    FactoryCameraProfile profile,
    const char *message);
void board_ui_show_status(
    const char *title,
    const char *detail,
    const char *footer = nullptr);
void board_ui_show_meter(
    const char *title,
    const char *detail,
    float left_dbfs,
    float right_dbfs);
void board_ui_show_display_pattern(unsigned pattern);
void board_ui_show_touch_grid(uint16_t hit_mask);
void board_ui_show_manual(
    FactoryTestId id,
    const char *title,
    const char *detail,
    bool allow_replay);
void board_ui_show_camera_frame(
    const uint16_t *pixels,
    uint16_t width,
    uint16_t height,
    FactoryTestId id,
    bool show_manual_buttons);
void board_ui_clear_camera_frame();
void board_ui_show_summary(const FactoryRunResult &result);
