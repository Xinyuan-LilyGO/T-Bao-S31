/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * app_guard_mode.c - 番茄时钟守护模式 backend.
 */
#include "app_guard_mode.h"

#include <string.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "app_activity.h"
#include "app_camera.h"
#include "app_camera_preview.h"
#include "app_vision_lite.h"

static const char *TAG = "GUARD_MODE";

#define GUARD_ANALYZE_MS      250
#define GUARD_NOBODY_ALERT_MS 60000
#define GUARD_STACK            (12 * 1024)

static SemaphoreHandle_t s_mutex;
static TaskHandle_t s_task;
static volatile bool s_running;
static volatile bool s_exited;

static uint8_t *s_frame;
static size_t s_frame_capacity;
static size_t s_frame_len;
static uint32_t s_frame_seq;

static guard_status_t s_status = GUARD_STATUS_IDLE;
static int s_last_ratio;
static uint32_t s_last_active_ms;
static uint32_t s_absent_start_ms;

static bool copy_pending_frame(uint8_t **decode_buf, size_t *decode_capacity,
                               size_t *decode_len, uint32_t *last_seq)
{
    bool ready = false;

    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    if (s_frame != NULL && s_frame_len > 0 && s_frame_seq != *last_seq) {
        if (*decode_capacity < s_frame_len) {
            uint8_t *new_buf = heap_caps_realloc(*decode_buf, s_frame_len,
                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (new_buf != NULL) {
                *decode_buf = new_buf;
                *decode_capacity = s_frame_len;
            }
        }
        if (*decode_buf != NULL && *decode_capacity >= s_frame_len) {
            memcpy(*decode_buf, s_frame, s_frame_len);
            *decode_len = s_frame_len;
            *last_seq = s_frame_seq;
            ready = true;
        }
    }
    xSemaphoreGive(s_mutex);
    return ready;
}

static void on_frame(const uint8_t *jpeg, size_t len, void *ctx)
{
    (void)ctx;
    if (!s_running || jpeg == NULL || len == 0 || s_mutex == NULL) {
        return;
    }
    if (xSemaphoreTake(s_mutex, 0) != pdTRUE) {
        return;
    }
    if (s_frame_capacity < len) {
        uint8_t *new_frame = heap_caps_realloc(s_frame, len,
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (new_frame != NULL) {
            s_frame = new_frame;
            s_frame_capacity = len;
        }
    }
    if (s_frame != NULL && s_frame_capacity >= len) {
        memcpy(s_frame, jpeg, len);
        s_frame_len = len;
        s_frame_seq++;
    }
    xSemaphoreGive(s_mutex);
}

static void guard_task(void *arg)
{
    (void)arg;
    uint8_t *decode_buf = NULL;
    size_t decode_capacity = 0;
    size_t decode_len = 0;
    uint8_t *output = heap_caps_aligned_alloc(64, CAM_PREVIEW_BYTES,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint32_t last_seq = 0;
    uint32_t last_analyze = 0;

    ESP_LOGI(TAG, "guard task started");
    if (output == NULL) {
        ESP_LOGE(TAG, "guard output buffer allocation failed");
        s_exited = true;
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    while (s_running) {
        if (!copy_pending_frame(&decode_buf, &decode_capacity, &decode_len, &last_seq)) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if (now - last_analyze < GUARD_ANALYZE_MS) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        last_analyze = now;

        if (app_camera_preview_process(decode_buf, decode_len, output) == ESP_OK) {
            app_vision_lite_feed_rgb565(output, CAM_PREVIEW_W, CAM_PREVIEW_H, now);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    heap_caps_free(decode_buf);
    heap_caps_free(output);
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    heap_caps_free(s_frame);
    s_frame = NULL;
    s_frame_capacity = 0;
    s_frame_len = 0;
    xSemaphoreGive(s_mutex);
    s_exited = true;
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t app_guard_mode_start(void)
{
    if (s_running) {
        return ESP_OK;
    }
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
        if (s_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    esp_err_t ret = app_vision_lite_init();
    if (ret != ESP_OK) {
        return ret;
    }
    app_activity_camera_acquire(APP_ACT_CAMERA_GUARD);
    app_vision_lite_set_enabled(true);

    s_running = true;
    s_exited = false;
    s_status = GUARD_STATUS_STARTING;
    s_last_active_ms = 0;
    s_absent_start_ms = 0;

    ret = app_camera_register_frame_cb(on_frame, NULL);
    if (ret != ESP_OK) {
        s_running = false;
        app_vision_lite_set_enabled(false);
        app_activity_camera_release(APP_ACT_CAMERA_GUARD);
        return ret;
    }

    BaseType_t task_ret = xTaskCreatePinnedToCoreWithCaps(
        guard_task, "guard", GUARD_STACK, NULL, 3, &s_task, 0,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "guard task create failed");
        app_camera_unregister_frame_cb(on_frame);
        app_vision_lite_set_enabled(false);
        app_activity_camera_release(APP_ACT_CAMERA_GUARD);
        s_running = false;
        return ESP_ERR_NO_MEM;
    }
    s_status = GUARD_STATUS_QUIET;
    ESP_LOGI(TAG, "guard mode started");
    return ESP_OK;
}

esp_err_t app_guard_mode_stop(void)
{
    if (!s_running) {
        return ESP_OK;
    }
    s_running = false;
    app_camera_unregister_frame_cb(on_frame);
    app_vision_lite_set_enabled(false);

    int wait_count = 0;
    while (!s_exited && wait_count++ < 150) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    s_task = NULL;
    s_exited = false;

    app_activity_camera_release(APP_ACT_CAMERA_GUARD);
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
        s_status = GUARD_STATUS_IDLE;
        s_last_ratio = 0;
        xSemaphoreGive(s_mutex);
    }
    ESP_LOGI(TAG, "guard mode stopped");
    return ESP_OK;
}

bool app_guard_mode_is_running(void)
{
    return s_running;
}

guard_info_t app_guard_mode_get_info(void)
{
    guard_info_t info;
    memset(&info, 0, sizeof(info));
    if (s_mutex == NULL) {
        return info;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    info.status = s_status;
    info.motion_ratio = s_last_ratio;
    info.absent_ms = s_absent_start_ms;
    xSemaphoreGive(s_mutex);
    return info;
}
