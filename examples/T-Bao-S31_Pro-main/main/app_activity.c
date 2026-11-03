/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * app_activity — demand-driven camera lifecycle (see app_activity.h).
 *
 * Consumers only ever manipulate a small need-mask (mutex-protected). The real
 * app_camera_start()/stop() calls happen in a single low-priority reconcile
 * task, which serialises camera power-up/down. The camera is stopped at the
 * next reconcile after the last need disappears, so idle pages do not retain
 * the capture pipeline or its PSRAM buffers.
 */
#include "app_activity.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "app_camera.h"

static const char *TAG = "ACT_MGR";

#define ACT_RECONCILE_MS    400     /* how often the reconcile task wakes */
#define ACT_WEB_LEASE_MS    2500    /* web lease: camera kept for 2.5s after a poll */
#define ACT_LINGER_MS       0       /* stop as soon as no consumer needs camera */
#define ACT_FAIL_COOLDOWN_MS 5000   /* don't spin-retry a broken camera */
/* Minimum time the camera must stay OFF after a stop before we may start it
 * again. The DVP driver needs a moment to fully release its DMA state after
 * STREAMOFF; the camera stop path now joins its receive task before this
 * cooldown starts. */
#define ACT_STOP_COOLDOWN_MS 500

static SemaphoreHandle_t s_mtx = NULL;
static uint32_t s_mask = 0;             /* static need bits (page/guard/rec) */
static uint64_t s_web_ts = 0;           /* last web touch (ms) */
static bool s_web_lease_active = false;
static uint64_t s_last_need = 0;        /* last ms the effective need was non-zero */
static uint64_t s_last_fail = 0;        /* last failed app_camera_start (ms) */
static uint64_t s_last_stop = 0;        /* last app_camera_stop (ms), for cooldown */
static bool s_start_failed = false;
static bool s_has_stopped = false;
static TaskHandle_t s_task = NULL;
static SemaphoreHandle_t s_wakeup = NULL;

static void signal_reconcile(void)
{
    if (s_wakeup != NULL) {
        (void)xSemaphoreGive(s_wakeup);
    }
}

static void act_task(void *arg)
{
    (void)arg;
    while (1) {
        /* A demand change wakes the manager immediately; the timeout remains
         * as a periodic repair pass for stale external state. */
        (void)xSemaphoreTake(s_wakeup, pdMS_TO_TICKS(ACT_RECONCILE_MS));
        uint64_t now = (uint64_t)(esp_timer_get_time() / 1000);

        xSemaphoreTake(s_mtx, portMAX_DELAY);
        uint32_t eff = s_mask;
        if (s_web_lease_active && now - s_web_ts < ACT_WEB_LEASE_MS) {
            eff |= APP_ACT_CAMERA_WEB;
        }
        if (eff != 0) {
            s_last_need = now;
        }
        bool running = app_camera_is_running();
        if (eff != 0) {
            if (!running && (!s_start_failed ||
                             (now - s_last_fail) >= ACT_FAIL_COOLDOWN_MS) &&
                (!s_has_stopped ||
                 (now - s_last_stop) >= ACT_STOP_COOLDOWN_MS)) {
                esp_err_t r = app_camera_start();
                if (r != ESP_OK) {
                    if (r == ESP_ERR_INVALID_STATE) {
                        /* A previous stop may still be completing its task
                         * join. Retry on the next reconcile instead of
                         * hiding that transient state behind a 5 s cooldown. */
                        ESP_LOGW(TAG, "camera start waiting for previous task: %s",
                                 esp_err_to_name(r));
                    } else {
                        s_last_fail = now;
                        s_start_failed = true;
                        ESP_LOGW(TAG, "camera start failed: %s",
                                 esp_err_to_name(r));
                    }
                } else {
                    s_start_failed = false;
                    ESP_LOGI(TAG, "camera started (need=0x%x)", (unsigned)eff);
                }
            }
#if ACT_LINGER_MS == 0
        } else if (running) {
#else
        } else if (running && (now - s_last_need) >= ACT_LINGER_MS) {
#endif
            const esp_err_t stop_ret = app_camera_stop();
            s_last_stop = now;
            s_has_stopped = true;
            ESP_LOGI(TAG, "camera stopped (idle, all apps silent): %s",
                     esp_err_to_name(stop_ret));
        }
        xSemaphoreGive(s_mtx);
    }
}

esp_err_t app_activity_init(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }
    if (s_mtx == NULL) {
        s_mtx = xSemaphoreCreateMutex();
        if (s_mtx == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_wakeup == NULL) {
        s_wakeup = xSemaphoreCreateBinary();
        if (s_wakeup == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    s_mask = 0;
    /* Deliberately do NOT arm the web lease at boot: a stale lease would start
     * the camera right after boot for no reason. It only becomes active after
     * the first real web request (app_activity_web_touch). */
    s_web_ts = 0;
    s_web_lease_active = false;
    s_last_need = 0;
    s_last_fail = 0;
    s_last_stop = 0;
    s_start_failed = false;
    s_has_stopped = false;
    BaseType_t t = xTaskCreatePinnedToCoreWithCaps(act_task, "act_mgr",
                                                   4096, NULL, 2, &s_task, 1,
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (t != pdPASS) {
        s_task = NULL;
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "activity manager started (camera runs only on demand)");
    return ESP_OK;
}

void app_activity_camera_acquire(app_activity_cam_reason_t r)
{
    if (s_mtx == NULL) {
        return;
    }
    uint32_t mask;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_mask |= (uint32_t)r;
    mask = s_mask;
    xSemaphoreGive(s_mtx);
    signal_reconcile();
    ESP_LOGI(TAG, "camera acquire reason=0x%x mask=0x%x",
             (unsigned)r, (unsigned)mask);
}

void app_activity_camera_release(app_activity_cam_reason_t r)
{
    if (s_mtx == NULL) {
        return;
    }
    uint32_t mask;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_mask &= ~(uint32_t)r;
    mask = s_mask;
    xSemaphoreGive(s_mtx);
    signal_reconcile();
    ESP_LOGI(TAG, "camera release reason=0x%x mask=0x%x",
             (unsigned)r, (unsigned)mask);
}

esp_err_t app_activity_camera_wait_until_running(uint32_t timeout_ms)
{
    if (s_mtx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const uint64_t start_ms = (uint64_t)(esp_timer_get_time() / 1000);
    while (!app_camera_is_running()) {
        const uint64_t elapsed_ms =
            (uint64_t)(esp_timer_get_time() / 1000) - start_ms;
        if (elapsed_ms >= timeout_ms) {
            return ESP_ERR_TIMEOUT;
        }
        uint32_t remaining_ms = timeout_ms - (uint32_t)elapsed_ms;
        if (remaining_ms > 20U) {
            remaining_ms = 20U;
        }
        vTaskDelay(pdMS_TO_TICKS(remaining_ms));
    }
    return ESP_OK;
}

void app_activity_web_touch(void)
{
    if (s_mtx == NULL) {
        return;
    }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_web_ts = (uint64_t)(esp_timer_get_time() / 1000);
    s_web_lease_active = true;
    xSemaphoreGive(s_mtx);
    signal_reconcile();
}

void app_activity_web_release(void)
{
    if (s_mtx == NULL) {
        return;
    }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_web_ts = 0;
    s_web_lease_active = false;
    xSemaphoreGive(s_mtx);
    signal_reconcile();
}
