/* T-Bao S31 OV2640/OV3660 JPEG camera wrapper. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_CAMERA_JPEG_QUALITY 70
#define APP_CAMERA_MAX_FRAME_CB 8

typedef void (*app_camera_frame_cb_t)(const uint8_t *jpeg, size_t len, void *ctx);

esp_err_t app_camera_start(void);
esp_err_t app_camera_stop(void);
esp_err_t app_camera_capture_jpeg(uint8_t **out_buf, size_t *out_len);
void app_camera_release_jpeg(uint8_t *buf);

/** Copy the shared latest JPEG into a caller-owned reusable buffer. */
esp_err_t app_camera_copy_latest_jpeg(uint8_t *out_buf, size_t out_capacity,
                                      size_t *out_len, uint32_t *out_sequence);

/** Read the size and sequence of the shared latest JPEG without copying it. */
esp_err_t app_camera_get_latest_jpeg_info(size_t *out_len, uint32_t *out_sequence);

esp_err_t app_camera_register_frame_cb(app_camera_frame_cb_t cb, void *ctx);
esp_err_t app_camera_unregister_frame_cb(app_camera_frame_cb_t cb);
bool app_camera_is_running(void);

#ifdef __cplusplus
}
#endif
