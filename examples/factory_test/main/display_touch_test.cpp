#include "factory_tests.h"

#include <cstdio>

#include "board_service.h"
#include "esp_timer.h"
#include "factory_settings.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

esp_err_t display_touch_test_run(
    const FactoryTestEnvironment &environment,
    FactoryTestRecord *record,
    FactoryModuleOutcome *outcome)
{
    if (record == nullptr || outcome == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *outcome = {};
    const FactoryBoardState &board = board_service_state();
    if (!board.display_ready) {
        std::snprintf(record->error_code, sizeof(record->error_code), "LCD_INIT_FAILED");
        std::snprintf(record->detail, sizeof(record->detail), "ST7796S is unavailable");
        return ESP_FAIL;
    }
    for (unsigned pattern = 0; pattern < 6; ++pattern) {
        board_ui_show_display_pattern(pattern);
        vTaskDelay(pdMS_TO_TICKS(450));
    }
    outcome->manual_required = true;
    outcome->replay_supported = true;
    if (!board.touch_ready || environment.wait_action == nullptr) {
        std::snprintf(record->error_code, sizeof(record->error_code), "I2C_NO_ACK");
        std::snprintf(record->detail, sizeof(record->detail), "FT6336 is unavailable");
        std::snprintf(record->measurements, sizeof(record->measurements),
                      "{\"touch_points\":0,\"i2c_errors\":1}");
        return ESP_FAIL;
    }

    board_service_reset_touch_error_count();
    uint16_t hit_mask = 0;
    board_ui_show_touch_grid(hit_mask);
    const int64_t deadline = esp_timer_get_time() +
        static_cast<int64_t>(FACTORY_TOUCH_TIMEOUT_MS) * 1000;
    while (hit_mask != 0x01FF && esp_timer_get_time() < deadline) {
        FactoryAction action = {};
        if (environment.wait_action(&action, 100, environment.wait_context) &&
            action.type == FactoryActionType::kTouchHit && action.value < 9) {
            hit_mask |= static_cast<uint16_t>(1U << action.value);
            board_ui_show_touch_grid(hit_mask);
        }
    }

    const uint32_t errors = board_service_touch_error_count();
    int hit_count = 0;
    for (int i = 0; i < 9; ++i) {
        hit_count += (hit_mask & (1U << i)) != 0;
    }
    std::snprintf(record->measurements, sizeof(record->measurements),
                  "{\"touch_points\":%d,\"i2c_errors\":%lu}", hit_count,
                  static_cast<unsigned long>(errors));
    if (hit_mask != 0x01FF) {
        std::snprintf(record->error_code, sizeof(record->error_code), "TIMEOUT");
        std::snprintf(record->detail, sizeof(record->detail),
                      "Touch grid incomplete (%d/9)", hit_count);
        return ESP_ERR_TIMEOUT;
    }
    if (errors != 0) {
        std::snprintf(record->error_code, sizeof(record->error_code), "I2C_READ_ERROR");
        std::snprintf(record->detail, sizeof(record->detail),
                      "Touch I2C errors: %lu", static_cast<unsigned long>(errors));
        return ESP_FAIL;
    }
    outcome->automatic_pass = true;
    std::snprintf(record->detail, sizeof(record->detail),
                  "9 touch points passed; verify display");
    return ESP_OK;
}

void display_touch_test_cleanup()
{
}
