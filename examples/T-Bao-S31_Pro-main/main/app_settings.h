/*
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool xiaozhi_enabled;
    bool ui_sounds_enabled;
    int volume;
    int brightness;
} app_settings_t;

/* Load saved values. Missing keys are replaced with safe defaults. */
esp_err_t app_settings_load(app_settings_t *settings);

/* Persist one setting without disturbing the other keys. */
esp_err_t app_settings_set_xiaozhi_enabled(bool enabled);
esp_err_t app_settings_set_ui_sounds_enabled(bool enabled);
esp_err_t app_settings_set_volume(int volume);
esp_err_t app_settings_set_brightness(int brightness);
esp_err_t app_settings_reset(void);

#ifdef __cplusplus
}
#endif
