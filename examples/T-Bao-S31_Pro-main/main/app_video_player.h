/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * SD-card MJPEG-in-MP4/AVI player for the T-Bao S31 LCD.
 */
#pragma once

#include "esp_err.h"
#include "app_video_overlay.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APP_VIDEO_PLAYER_PATH_MAX 300

typedef enum {
    APP_VIDEO_PLAYER_STOPPED = 0,
    APP_VIDEO_PLAYER_PLAYING,
    APP_VIDEO_PLAYER_PAUSED,
} app_video_player_state_t;

/** Register the Espressif default MJPEG/H.264 decoders and create controls. */
esp_err_t app_video_player_init(void);

/** Start one local MP4/AVI file, using a POSIX path such as /sdcard/a.avi. */
esp_err_t app_video_player_play(const char *path);

esp_err_t app_video_player_pause(void);
esp_err_t app_video_player_resume(void);
esp_err_t app_video_player_stop(void);

/** Publish direct-video controls and wake the renderer for a redraw. */
esp_err_t app_video_player_set_overlay_state(
    const app_video_overlay_state_t *state);

app_video_player_state_t app_video_player_get_state(void);

/** Return the last player error, or ESP_OK when the last run ended normally. */
esp_err_t app_video_player_last_error(void);

#ifdef __cplusplus
}
#endif
