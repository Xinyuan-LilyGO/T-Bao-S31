/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ui_touch.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_board_manager.h"
#include "dev_lcd_touch.h"
#include "esp_lcd_touch.h"

static const char *TAG = "UI_TOUCH";

#define TOUCH_POLL_MS   10
#define TOUCH_RELEASE_DEBOUNCE_SAMPLES 12
#define TOUCH_STACK     2048
#define TOUCH_PRIO      6

static esp_lcd_touch_handle_t s_touch = NULL;
static ui_touch_cb_t s_cb = NULL;
static TaskHandle_t s_task = NULL;
static TaskHandle_t s_disp_task = NULL;
static QueueHandle_t s_gesture_queue = NULL;
static bool s_running = false;
/* The controller is sampled during boot so diagnostics remain useful, but
 * input must stay isolated until the LVGL/emote/page ownership handoff ends. */
static volatile bool s_input_enabled = false;

/* Gestures are classified in the polling touch task but DISPATCHED in a separate
 * task. This keeps the poller free to keep feeding LVGL pointer input even while
 * a page switch (which can take a while under the LVGL lock) is in progress,
 * so the UI never feels frozen / unresponsive. */
typedef struct {
    ui_gesture_t g;
    int x;
    int y;
} gesture_msg_t;

/* Per-contact tracking state. */
static bool     s_down = false;
static int      s_down_x = 0;
static int      s_down_y = 0;
static int64_t  s_down_ms = 0;
static int64_t  s_last_touch_ms = 0;
static int      s_last_x = 0;
static int      s_last_y = 0;

typedef struct {
    int ui_x;
    int ui_y;
    int lvgl_x;
    int lvgl_y;
} touch_coordinates_t;

/* LVGL receives coordinates before its display rotation. Gestures use the
 * same logical coordinates as the widgets visible to the user. */
static volatile int  s_cur_x = -1;
static volatile int  s_cur_y = -1;
static volatile bool s_cur_pressed = false;

#if CONFIG_ESP_BOARD_TBAO_S31_BOARD
static void map_touch_coordinates(int touch_x, int touch_y,
                                  touch_coordinates_t *coordinates)
{
    const int max_x = s_touch != NULL ? (int)s_touch->config.x_max : 319;
    const int max_y = s_touch != NULL ? (int)s_touch->config.y_max : 319;

    if (touch_x < 0) {
        touch_x = 0;
    } else if (touch_x > max_x) {
        touch_x = max_x;
    }
    if (touch_y < 0) {
        touch_y = 0;
    } else if (touch_y > max_y) {
        touch_y = max_y;
    }

    coordinates->ui_x = touch_y;
    coordinates->ui_y = touch_x;
    coordinates->lvgl_x = max_x - touch_x;
    coordinates->lvgl_y = touch_y;
}
#else
static void map_touch_coordinates(int touch_x, int touch_y,
                                  touch_coordinates_t *coordinates)
{
    coordinates->ui_x = touch_x;
    coordinates->ui_y = touch_y;
    coordinates->lvgl_x = touch_x;
    coordinates->lvgl_y = touch_y;
}
#endif

static int64_t now_ms(void)
{
    return (int64_t)(esp_timer_get_time() / 1000);
}

static void emit(ui_gesture_t g, int x, int y)
{
    if (!s_input_enabled) {
        return;
    }
    if (s_gesture_queue == NULL) {
        /* Queue not ready yet (early boot) — dispatch directly as a fallback. */
        if (s_cb) {
            s_cb(g, x, y);
        }
        return;
    }
    gesture_msg_t msg = { g, x, y };
    /* Best-effort enqueue; drop if the consumer is somehow behind. */
    xQueueSend(s_gesture_queue, &msg, 0);
}

static void ui_touch_dispatch_task(void *arg)
{
    (void)arg;
    gesture_msg_t msg;
    while (1) {
        if (xQueueReceive(s_gesture_queue, &msg, portMAX_DELAY) == pdTRUE) {
            if (s_cb) {
                s_cb(msg.g, msg.x, msg.y);
            }
        }
    }
}

static void classify_release(void)
{
    int64_t dt = s_last_touch_ms - s_down_ms;
    if (dt < 0) {
        dt = 0;
    }
    int dx = s_last_x - s_down_x;
    int dy = s_last_y - s_down_y;
    int adx = dx < 0 ? -dx : dx;
    int ady = dy < 0 ? -dy : dy;

    if (dt >= UI_LONG_PRESS_MS && adx < UI_SWIPE_MIN_DIST_PX && ady < UI_SWIPE_MIN_DIST_PX) {
        emit(UI_GESTURE_LONG_PRESS, s_down_x, s_down_y);
    } else if (adx >= UI_SWIPE_MIN_DIST_PX || ady >= UI_SWIPE_MIN_DIST_PX) {
        if (adx > ady) {
            emit(dx > 0 ? UI_GESTURE_SWIPE_RIGHT : UI_GESTURE_SWIPE_LEFT,
                 s_down_x, s_down_y);
        } else {
            emit(dy > 0 ? UI_GESTURE_SWIPE_DOWN : UI_GESTURE_SWIPE_UP,
                 s_down_x, s_down_y);
        }
    } else if (dt <= UI_TAP_MAX_MS) {
        emit(UI_GESTURE_TAP, s_down_x, s_down_y);
    }
}

static void touch_task(void *arg)
{
    (void)arg;
    esp_lcd_touch_point_data_t pts[1];
    uint8_t empty_samples = 0;
    bool read_error_logged = false;

    while (s_running) {
        if (s_touch == NULL) {
            vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_MS));
            continue;
        }
        uint8_t point_cnt = 0;
        esp_err_t ret = esp_lcd_touch_read_data(s_touch);
        if (ret != ESP_OK) {
            if (!read_error_logged) {
                ESP_LOGW(TAG, "touch read failed: %s", esp_err_to_name(ret));
                read_error_logged = true;
            }
            vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_MS));
            continue;
        }
        ret = esp_lcd_touch_get_data(s_touch, pts, &point_cnt, 1);
        if (ret != ESP_OK) {
            if (!read_error_logged) {
                ESP_LOGW(TAG, "touch decode failed: %s", esp_err_to_name(ret));
                read_error_logged = true;
            }
            vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_MS));
            continue;
        }
        if (read_error_logged) {
            ESP_LOGI(TAG, "touch read recovered");
            read_error_logged = false;
        }

        if (point_cnt > 0) {
            touch_coordinates_t coordinates;
            map_touch_coordinates(pts[0].x, pts[0].y, &coordinates);
            empty_samples = 0;
            s_last_x = coordinates.ui_x;
            s_last_y = coordinates.ui_y;
            s_cur_x = coordinates.lvgl_x;
            s_cur_y = coordinates.lvgl_y;
            s_cur_pressed = true;
            s_last_touch_ms = now_ms();
            if (!s_down) {
                s_down = true;
                s_down_x = coordinates.ui_x;
                s_down_y = coordinates.ui_y;
                s_down_ms = s_last_touch_ms;
                ESP_LOGI(TAG, "touch down: touch=(%d,%d) ui=(%d,%d) lvgl=(%d,%d)",
                         pts[0].x, pts[0].y,
                         coordinates.ui_x, coordinates.ui_y,
                         coordinates.lvgl_x, coordinates.lvgl_y);
            }
        } else if (s_down) {
            if (empty_samples < TOUCH_RELEASE_DEBOUNCE_SAMPLES) {
                ++empty_samples;
            }
            if (empty_samples >= TOUCH_RELEASE_DEBOUNCE_SAMPLES) {
                /* Require consecutive empty samples before releasing. */
                s_down = false;
                s_cur_pressed = false;
                empty_samples = 0;
                classify_release();
            }
        } else {
            s_cur_pressed = false;
        }
        vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_MS));
    }
    vTaskDelete(NULL);
}

esp_err_t ui_touch_init(void)
{
    if (s_running) {
        return ESP_OK;
    }
    void *handle = NULL;
    esp_err_t ret = esp_board_manager_get_device_handle("lcd_touch", &handle);
    if (ret != ESP_OK || handle == NULL) {
        ESP_LOGW(TAG, "lcd_touch handle unavailable (%s); touch disabled",
                 esp_err_to_name(ret));
        return ret;
    }
    dev_lcd_touch_handles_t *h = (dev_lcd_touch_handles_t *)handle;
    if (h->touch_handle == NULL) {
        ESP_LOGW(TAG, "lcd_touch handle has NULL touch_handle; touch disabled");
        return ESP_ERR_INVALID_STATE;
    }
    s_touch = h->touch_handle;
    s_input_enabled = false;
    s_down = false;
    s_cur_pressed = false;

    if (s_gesture_queue == NULL) {
        s_gesture_queue = xQueueCreate(8, sizeof(gesture_msg_t));
        if (s_gesture_queue == NULL) {
            ESP_LOGE(TAG, "failed to create gesture queue");
            return ESP_FAIL;
        }
    }

    s_running = true;
    BaseType_t t = xTaskCreate(touch_task, "ui_touch", TOUCH_STACK, NULL, TOUCH_PRIO, &s_task);
    if (t != pdPASS) {
        s_running = false;
        ESP_LOGE(TAG, "failed to create touch task");
        return ESP_FAIL;
    }
    BaseType_t dt = xTaskCreate(ui_touch_dispatch_task, "ui_touch_disp", 3072, NULL, 5, &s_disp_task);
    if (dt != pdPASS) {
        ESP_LOGE(TAG, "failed to create gesture dispatch task");
        /* Non-fatal: gestures would just be dropped; keep the touch poller. */
    }
    ESP_LOGI(TAG, "touch layer ready");
    return ESP_OK;
}

void ui_touch_set_callback(ui_touch_cb_t cb)
{
    s_cb = cb;
}

void ui_touch_set_enabled(bool enabled)
{
    /* A contact held across the boot boundary must not become a tap/swipe
     * after initialization merely because its release is observed later. */
    if (enabled && !s_input_enabled) {
        s_down = false;
        s_cur_pressed = false;
        s_down_ms = 0;
        s_last_touch_ms = 0;
        s_last_x = 0;
        s_last_y = 0;
    }
    s_input_enabled = enabled;
    if (!enabled) {
        s_down = false;
        s_cur_pressed = false;
    }
}

void ui_touch_get_pos(int *x, int *y, bool *pressed)
{
    if (x) {
        /* LVGL also consumes the last point during LV_EVENT_RELEASED. Keep
         * the coordinate after release so a slider does not recompute from
         * the synthetic (0, 0) point. */
        *x = (int)s_cur_x;
    }
    if (y) {
        *y = (int)s_cur_y;
    }
    if (pressed) {
        *pressed = s_input_enabled && s_cur_pressed;
    }
}

esp_err_t ui_touch_deinit(void)
{
    s_input_enabled = false;
    if (s_task) {
        s_running = false;
        vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_MS * 2));
        s_task = NULL;
    }
    if (s_disp_task) {
        vTaskDelete(s_disp_task);
        s_disp_task = NULL;
    }
    if (s_gesture_queue) {
        vQueueDelete(s_gesture_queue);
        s_gesture_queue = NULL;
    }
    s_touch = NULL;
    s_cb = NULL;
    return ESP_OK;
}
