/* Hardware JPEG decoder used by the T-Bao S31 camera preview. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CAM_PREVIEW_W 320
#define CAM_PREVIEW_H 320
#define CAM_PREVIEW_BYTES (CAM_PREVIEW_W * CAM_PREVIEW_H * 2)

esp_err_t app_camera_preview_init(void);
esp_err_t app_camera_preview_process(const uint8_t *jpeg, size_t jpeg_len, uint8_t *out_rgb565);
void app_camera_preview_deinit(void);

#ifdef __cplusplus
}
#endif
