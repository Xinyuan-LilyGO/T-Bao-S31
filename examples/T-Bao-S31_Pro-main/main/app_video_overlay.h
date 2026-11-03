/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * LVGL-free RGB565 controls for the direct video renderer.
 */
#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_VIDEO_OVERLAY_MODE_SINGLE_LOOP = 0,
    APP_VIDEO_OVERLAY_MODE_RANDOM,
    APP_VIDEO_OVERLAY_MODE_SEQUENTIAL,
} app_video_overlay_mode_t;

typedef enum {
    APP_VIDEO_OVERLAY_ACTION_NONE = 0,
    APP_VIDEO_OVERLAY_ACTION_PAUSE,
    APP_VIDEO_OVERLAY_ACTION_PLAY,
} app_video_overlay_action_t;

typedef struct {
    bool controls_visible;
    bool paused;
    bool show_action_badge;
    bool show_mode_badge;
    app_video_overlay_mode_t mode;
    app_video_overlay_action_t action;
} app_video_overlay_state_t;

esp_err_t app_video_overlay_init(int width, int height);
void app_video_overlay_set_state(const app_video_overlay_state_t *state);
esp_err_t app_video_overlay_compose(const uint16_t *base,
                                    uint16_t *composed,
                                    int width, int height,
                                    uint32_t now_ms);
void app_video_overlay_deinit(void);

#ifdef __cplusplus
}
#endif
