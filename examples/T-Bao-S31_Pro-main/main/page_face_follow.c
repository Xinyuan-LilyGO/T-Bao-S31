/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Dedicated offline face-follow page. The camera preview is fed from the
 * normal JPEG frame callback while the existing face-follow task owns model
 * inference and motion control. The two paths share the same 320x320 frame
 * coordinates, so the tracked target can be drawn directly over the canvas.
 */
#include "page_face_follow.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "app_activity.h"
#include "app_camera.h"
#include "app_camera_preview.h"
#include "app_face_follow.h"
#include "app_language.h"
#include "app_sfx.h"
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "esp_xiaozhi_chat_display.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "font_awesome_symbols.h"
#include "lvgl.h"
#include "motion_manager.h"
#include "page_launcher.h"
#include "ui_page_style.h"
#include "launcher_icons.h"

#define TAG "PAGE_FACE_FOLLOW"

#define PREVIEW_TASK_STACK  (12 * 1024)
/* Poll faster than the camera frame interval (~47 ms at 21 fps) so each new
 * JPEG is picked up promptly instead of waiting for the next 45 ms slot. */
#define PREVIEW_POLL_MS     20U
#define PAGE_REFRESH_MS     100U
#define FACE_BOX_COLOR      0x34C759
#define FACE_BOX_BORDER_PX  3

LV_FONT_DECLARE(font_puhui_16_4);
static lv_obj_t *s_canvas;
static lv_color16_t *s_canvas_buf;
static lv_obj_t *s_face_box;
static lv_obj_t *s_status_label;
static lv_obj_t *s_action_button;
static lv_obj_t *s_action_button_label;
static lv_obj_t *s_background_popup;
static lv_timer_t *s_background_popup_timer;

static SemaphoreHandle_t s_frame_mutex;
static uint8_t *s_frame;
static size_t s_frame_capacity;
static size_t s_frame_len;
static uint32_t s_frame_seq;
static TaskHandle_t s_preview_task;
static SemaphoreHandle_t s_preview_exit_sem;
static volatile bool s_preview_running;
static volatile uint32_t s_preview_generation;
static volatile uint32_t s_preview_task_generation;
static volatile uint32_t s_preview_done_generation;
static uint32_t s_received_frame_count;
static uint32_t s_rendered_frame_count;

static face_follow_state_t s_last_ui_state;
static bool s_has_last_ui_state;
static uint32_t s_last_refresh_ms;

static void hide_background_popup(void)
{
    if (s_background_popup_timer != NULL) {
        lv_timer_delete(s_background_popup_timer);
        s_background_popup_timer = NULL;
    }
    if (s_background_popup != NULL) {
        lv_obj_del(s_background_popup);
        s_background_popup = NULL;
    }
}

static void background_popup_timer_cb(lv_timer_t *timer)
{
    if (s_background_popup_timer == timer) {
        s_background_popup_timer = NULL;
    }
    lv_timer_delete(timer);
    if (s_background_popup != NULL) {
        lv_obj_del(s_background_popup);
        s_background_popup = NULL;
    }
}

static void background_popup_ok_cb(lv_event_t *event)
{
    (void)event;
    app_sfx_play(APP_SFX_TAP);
    hide_background_popup();
}

static void show_background_popup(void)
{
    lv_obj_t *root = esp_xiaozhi_chat_display_page_root();
    if (root == NULL) {
        return;
    }

    hide_background_popup();
    s_background_popup = lv_obj_create(root);
    if (s_background_popup == NULL) {
        return;
    }
    lv_obj_set_size(s_background_popup, LV_HOR_RES - 36, 164);
    lv_obj_center(s_background_popup);
    ui_page_style_panel(s_background_popup);
    lv_obj_set_style_pad_all(s_background_popup, 12, 0);
    lv_obj_clear_flag(s_background_popup, LV_OBJ_FLAG_SCROLLABLE |
                                             LV_OBJ_FLAG_SCROLL_CHAIN);

    lv_obj_t *label = lv_label_create(s_background_popup);
    if (label == NULL) {
        hide_background_popup();
        return;
    }
    lv_obj_set_width(label, LV_HOR_RES - 60);
    lv_obj_set_height(label, 88);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(label,
                      app_language_text(
                          "后台运行已开启。返回表情主页、表情选择或 App 桌面后仍会继续跟随；进入小智 AI 或摄像头前，请先停止跟随。",
                          "Background follow is on. It continues on the face, expression, and app pages. Stop it before opening Xiaozhi AI or Camera."));
    lv_obj_set_style_text_font(label, &font_puhui_16_4, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 4);

    lv_obj_t *button = lv_btn_create(s_background_popup);
    if (button == NULL) {
        hide_background_popup();
        return;
    }
    lv_obj_set_size(button, 92, 34);
    lv_obj_align(button, LV_ALIGN_BOTTOM_MID, 0, -8);
    ui_page_style_button(button, 0x34C759, 0x269A43, 16);
    lv_obj_add_event_cb(button, background_popup_ok_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *button_label = lv_label_create(button);
    lv_label_set_text(button_label, app_language_text("知道了", "OK"));
    lv_obj_set_style_text_font(button_label, &font_puhui_16_4, 0);
    lv_obj_set_style_text_color(button_label, lv_color_white(), 0);
    lv_obj_center(button_label);

    s_background_popup_timer = lv_timer_create(
        background_popup_timer_cb, 5000, NULL);
    if (s_background_popup_timer == NULL) {
        hide_background_popup();
    }
}

static void clear_frame_buffer(void)
{
    if (s_frame_mutex == NULL ||
        xSemaphoreTake(s_frame_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    heap_caps_free(s_frame);
    s_frame = NULL;
    s_frame_capacity = 0;
    s_frame_len = 0;
    s_frame_seq = 0;
    xSemaphoreGive(s_frame_mutex);
}

/* Page callbacks run while the LVGL port is locked. Release that lock while
 * joining the worker so a frame that is already entering LVGL cannot deadlock
 * the page switch. */
static esp_err_t preview_task_wait_for_exit(void)
{
    const uint32_t task_generation = s_preview_task_generation;
    if (task_generation == 0 ||
        (s_preview_task == NULL &&
         s_preview_done_generation == task_generation) ||
        s_preview_task == xTaskGetCurrentTaskHandle()) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "waiting for preview task to exit");
    lvgl_port_unlock();
    bool exited = false;
    int wait_count = 0;
    while (wait_count++ < 350) {
        if (s_preview_done_generation == task_generation) {
            exited = true;
            break;
        }
        if (s_preview_exit_sem != NULL) {
            (void)xSemaphoreTake(s_preview_exit_sem, pdMS_TO_TICKS(10));
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    while (!lvgl_port_lock(1000)) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!exited || s_preview_task == NULL) {
        ESP_LOGE(TAG, "preview task did not exit in time");
        return ESP_ERR_TIMEOUT;
    }
    TaskHandle_t task = s_preview_task;
    vTaskSuspend(task);
    vTaskDeleteWithCaps(task);
    s_preview_task = NULL;
    return ESP_OK;
}

static void signal_preview_task_exit(TaskHandle_t task, uint32_t generation)
{
    (void)task;
    clear_frame_buffer();
    s_preview_done_generation = generation;
    if (s_preview_exit_sem != NULL) {
        (void)xSemaphoreGive(s_preview_exit_sem);
    }
}

static bool copy_pending_frame(uint8_t **decode_buf,
                               size_t *decode_capacity,
                               size_t *decode_len,
                               uint32_t *last_seq)
{
    bool ready = false;

    if (xSemaphoreTake(s_frame_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    if (s_frame != NULL && s_frame_len > 0 && s_frame_seq != *last_seq) {
        if (*decode_capacity < s_frame_len) {
            uint8_t *new_buf = heap_caps_realloc(
                *decode_buf, s_frame_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
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
    xSemaphoreGive(s_frame_mutex);
    return ready;
}

static void on_frame(const uint8_t *jpeg, size_t len, void *ctx)
{
    const uint32_t generation = (uint32_t)(uintptr_t)ctx;
    if (!s_preview_running || generation != s_preview_generation ||
        jpeg == NULL || len == 0 ||
        s_frame_mutex == NULL) {
        return;
    }
    if (xSemaphoreTake(s_frame_mutex, pdMS_TO_TICKS(2)) != pdTRUE) {
        return;
    }
    uint32_t frame_number = 0;
    if (s_frame_capacity < len) {
        uint8_t *new_frame = heap_caps_realloc(
            s_frame, len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (new_frame != NULL) {
            s_frame = new_frame;
            s_frame_capacity = len;
        }
    }
    if (s_frame != NULL && s_frame_capacity >= len) {
        memcpy(s_frame, jpeg, len);
        s_frame_len = len;
        s_frame_seq++;
        s_received_frame_count++;
        frame_number = s_received_frame_count;
    }
    xSemaphoreGive(s_frame_mutex);
    if (frame_number == 1 || (frame_number % 30U) == 0U) {
        ESP_LOGI(TAG, "preview callback received JPEG #%u len=%u",
                 (unsigned)frame_number, (unsigned)len);
    }
}

static void preview_task(void *arg)
{
    const uint32_t generation = (uint32_t)(uintptr_t)arg;
    const TaskHandle_t task = xTaskGetCurrentTaskHandle();
    uint8_t *decode_buf = NULL;
    size_t decode_capacity = 0;
    size_t decode_len = 0;
    uint8_t *output = heap_caps_aligned_alloc(
        64, CAM_PREVIEW_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint32_t last_seq = 0;

    if (output == NULL) {
        ESP_LOGE(TAG, "preview buffer allocation failed");
        if (s_preview_task == task) {
            s_preview_running = false;
        }
        signal_preview_task_exit(task, generation);
        for (;;) {
            vTaskSuspend(NULL);
        }
    }

    uint32_t camera_wait_ms = 0;
    while (s_preview_running && generation == s_preview_generation &&
           !app_camera_is_running() && camera_wait_ms < 5000U) {
        const esp_err_t wait_ret =
            app_activity_camera_wait_until_running(20U);
        if (wait_ret != ESP_OK && wait_ret != ESP_ERR_TIMEOUT) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        camera_wait_ms += 20U;
    }
    if (!app_camera_is_running()) {
        ESP_LOGW(TAG, "preview started without a running camera after %ums",
                 (unsigned)camera_wait_ms);
    } else {
        ESP_LOGI(TAG, "preview camera is running after %ums",
                 (unsigned)camera_wait_ms);
    }

    uint32_t decode_error_count = 0;
    while (s_preview_running && generation == s_preview_generation) {
        if (!copy_pending_frame(&decode_buf, &decode_capacity, &decode_len,
                                &last_seq)) {
            vTaskDelay(pdMS_TO_TICKS(PREVIEW_POLL_MS));
            continue;
        }

        /* Decode outside the LVGL lock: the inference task shares the JPEG
         * decoder mutex, and waiting for it while holding the LVGL lock stalls
         * rendering (the canvas would stop updating entirely). */
        const esp_err_t decode_ret =
            app_camera_preview_process(decode_buf, decode_len, output);
        if (decode_ret == ESP_OK) {
            if (s_preview_running && generation == s_preview_generation) {
                if (lvgl_port_lock(1000)) {
                    if (s_preview_running && generation == s_preview_generation &&
                        s_canvas != NULL && s_canvas_buf != NULL) {
                        memcpy(s_canvas_buf, output, CAM_PREVIEW_BYTES);
                        lv_obj_invalidate(s_canvas);
                        s_rendered_frame_count++;
                        if (s_rendered_frame_count == 1 ||
                            (s_rendered_frame_count % 30U) == 0U) {
                            ESP_LOGI(TAG, "preview rendered RGB565 #%u",
                                     (unsigned)s_rendered_frame_count);
                        }
                    }
                    lvgl_port_unlock();
                }
            }
        } else if (decode_error_count++ == 0 ||
                   (decode_error_count % 30U) == 0U) {
            ESP_LOGW(TAG, "preview JPEG decode failed #%u: %s",
                     (unsigned)decode_error_count, esp_err_to_name(decode_ret));
        }
        vTaskDelay(pdMS_TO_TICKS(PREVIEW_POLL_MS));
    }

    heap_caps_free(decode_buf);
    heap_caps_free(output);
    signal_preview_task_exit(task, generation);
    for (;;) {
        vTaskSuspend(NULL);
    }
}

static const char *follow_state_text(face_follow_state_t state)
{
    switch (state) {
    case FACE_FOLLOW_STATE_STARTING:
        return app_language_text("启动中", "Starting");
    case FACE_FOLLOW_STATE_ACQUIRING:
        return app_language_text("等待人脸", "Waiting for face");
    case FACE_FOLLOW_STATE_FOLLOWING:
        return app_language_text("跟随中", "Following");
    case FACE_FOLLOW_STATE_LOST:
        return app_language_text("人脸丢失，已停车", "Face lost; stopped");
    case FACE_FOLLOW_STATE_ERROR:
        return app_language_text("跟随故障", "Follow fault");
    case FACE_FOLLOW_STATE_DISABLED:
    default:
        return app_language_text("已停止", "Stopped");
    }
}

static int clamp_int(int value, int minimum, int maximum)
{
    if (value < minimum) {
        return minimum;
    }
    if (value > maximum) {
        return maximum;
    }
    return value;
}

static void update_face_box(const face_follow_status_t *status)
{
    if (s_face_box == NULL || status == NULL || !status->enabled ||
        !status->target.valid) {
        if (s_face_box != NULL) {
            lv_obj_add_flag(s_face_box, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    int x1 = clamp_int((int)lroundf(status->target.x1), 0, CAM_PREVIEW_W - 1);
    int y1 = clamp_int((int)lroundf(status->target.y1), 0, CAM_PREVIEW_H - 1);
    int x2 = clamp_int((int)lroundf(status->target.x2), x1 + 1, CAM_PREVIEW_W);
    int y2 = clamp_int((int)lroundf(status->target.y2), y1 + 1, CAM_PREVIEW_H);

    if (x2 <= x1) {
        x2 = clamp_int(x1 + 4, 1, CAM_PREVIEW_W);
    }
    if (y2 <= y1) {
        y2 = clamp_int(y1 + 4, 1, CAM_PREVIEW_H);
    }

    lv_obj_set_pos(s_face_box, x1, y1);
    lv_obj_set_size(s_face_box, x2 - x1, y2 - y1);
    lv_obj_clear_flag(s_face_box, LV_OBJ_FLAG_HIDDEN);
}

static void refresh_page_state(void)
{
    face_follow_status_t status;
    if (app_face_follow_get_state(&status) != ESP_OK) {
        return;
    }

    update_face_box(&status);
    if (s_action_button_label != NULL) {
        const bool background =
            status.enabled && app_face_follow_is_background_enabled();
        lv_label_set_text(
            s_action_button_label,
            status.enabled ? (background
                                  ? app_language_text("停止跟随", "Stop follow")
                                  : app_language_text("后台运行", "Background"))
                           : app_language_text("开始", "Start"));
        if (s_action_button != NULL) {
            ui_page_style_button(s_action_button,
                                 background ? 0xE5484D : 0x34C759,
                                 background ? 0xB83232 : 0x269A43, 8);
        }
    }
    if (s_status_label != NULL) {
        if (status.target.valid) {
            if (app_language_is_english()) {
                lv_label_set_text_fmt(
                    s_status_label, "%s%s  Box %.0f,%.0f - %.0f,%.0f",
                    follow_state_text(status.state),
                    status.target.predicted ? "  (predicted)" : "",
                    status.target.x1, status.target.y1, status.target.x2,
                    status.target.y2);
            } else {
                lv_label_set_text_fmt(
                    s_status_label, "%s%s  框 %.0f,%.0f - %.0f,%.0f",
                    follow_state_text(status.state),
                    status.target.predicted ? " 预测" : "", status.target.x1,
                    status.target.y1, status.target.x2, status.target.y2);
            }
        } else if (status.state == FACE_FOLLOW_STATE_ERROR &&
                   status.last_error != ESP_OK) {
            lv_label_set_text_fmt(s_status_label, "%s: %s",
                                  follow_state_text(status.state),
                                  esp_err_to_name(status.last_error));
        } else {
            lv_label_set_text(s_status_label, follow_state_text(status.state));
        }
    }

    if (!s_has_last_ui_state || status.state != s_last_ui_state) {
        if (status.target.valid) {
            ESP_LOGI(TAG,
                     "UI state=%s source=%s box=(%.0f,%.0f)-(%.0f,%.0f) "
                     "score=%.2f",
                     follow_state_text(status.state),
                     status.target.predicted ? "predicted" : "measured",
                     status.target.x1,
                     status.target.y1, status.target.x2, status.target.y2,
                     status.target.score);
        } else {
            ESP_LOGI(TAG, "UI state=%s face=none", follow_state_text(status.state));
        }
        s_last_ui_state = status.state;
        s_has_last_ui_state = true;
    }
}

static void action_button_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    app_sfx_play(APP_SFX_TAP);
    esp_err_t ret;
    if (app_face_follow_is_enabled()) {
        if (app_face_follow_is_background_enabled()) {
            ret = app_face_follow_stop();
            motion_manager_stop_all();
            hide_background_popup();
            ESP_LOGI(TAG, "user stopped background face follow; vehicle parked");
        } else {
            ret = app_face_follow_set_background_enabled(true);
            if (ret == ESP_OK) {
                show_background_popup();
                ESP_LOGI(TAG, "user enabled background face follow");
            }
        }
    } else {
        ret = app_face_follow_start();
        ESP_LOGI(TAG, "user requested face follow restart: %s",
                 esp_err_to_name(ret));
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "face follow action failed: %s", esp_err_to_name(ret));
    }
    refresh_page_state();
}

static esp_err_t build_page(void)
{
    lv_obj_t *root = esp_xiaozhi_chat_display_page_root();
    if (root == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    ui_page_style_root(root);

    if (s_canvas_buf == NULL) {
        s_canvas_buf = heap_caps_aligned_alloc(
            64, CAM_PREVIEW_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_canvas_buf == NULL) {
            return ESP_ERR_NO_MEM;
        }
        memset(s_canvas_buf, 0, CAM_PREVIEW_BYTES);
    }

    s_canvas = lv_canvas_create(root);
    if (s_canvas == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_canvas_set_buffer(s_canvas, s_canvas_buf, CAM_PREVIEW_W, CAM_PREVIEW_H,
                         LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(s_canvas, 0, 0);

    s_face_box = lv_obj_create(root);
    if (s_face_box == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_pos(s_face_box, 0, 0);
    lv_obj_set_size(s_face_box, 4, 4);
    lv_obj_set_style_bg_opa(s_face_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_face_box, FACE_BOX_BORDER_PX, 0);
    lv_obj_set_style_border_color(s_face_box, lv_color_hex(FACE_BOX_COLOR), 0);
    lv_obj_set_style_border_opa(s_face_box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_face_box, 0, 0);
    lv_obj_set_style_shadow_width(s_face_box, 0, 0);
    lv_obj_set_style_pad_all(s_face_box, 0, 0);
    lv_obj_clear_flag(s_face_box, LV_OBJ_FLAG_CLICKABLE |
                                  LV_OBJ_FLAG_SCROLLABLE |
                                  LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_flag(s_face_box, LV_OBJ_FLAG_IGNORE_LAYOUT |
                                  LV_OBJ_FLAG_HIDDEN);

    ui_page_make_back_button(root);
    ui_page_make_header_image(root, &launcher_icon_face_follow, "人脸跟随",
                              "绿色框为当前目标");

    s_status_label = lv_label_create(root);
    if (s_status_label == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_style_text_font(s_status_label, &font_puhui_16_4, 0);
    lv_obj_set_style_text_color(s_status_label, lv_color_white(), 0);
    lv_obj_set_style_text_align(s_status_label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_width(s_status_label, LV_HOR_RES - 128);
    lv_obj_set_pos(s_status_label, 8, LV_VER_RES - 34);
    lv_label_set_long_mode(s_status_label, LV_LABEL_LONG_DOT);

    s_action_button = lv_btn_create(root);
    if (s_action_button == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_size(s_action_button, 108, 30);
    lv_obj_set_pos(s_action_button, LV_HOR_RES - 116, LV_VER_RES - 36);
    ui_page_style_button(s_action_button, 0x34C759, 0x269A43, 8);
    lv_obj_add_event_cb(s_action_button, action_button_cb, LV_EVENT_CLICKED, NULL);
    s_action_button_label = lv_label_create(s_action_button);
    if (s_action_button_label == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_style_text_font(s_action_button_label, &font_puhui_16_4, 0);
    lv_obj_set_style_text_color(s_action_button_label, lv_color_white(), 0);
    lv_label_set_text(s_action_button_label,
                      app_language_text("后台运行", "Background"));
    lv_obj_center(s_action_button_label);
    return ESP_OK;
}

esp_err_t page_face_follow_on_enter(void)
{
    app_sfx_play(APP_SFX_PAGE);
    ESP_LOGI(TAG, "enter page: preview and offline follow requested");

    if (s_frame_mutex == NULL) {
        s_frame_mutex = xSemaphoreCreateMutex();
    }
    if (s_frame_mutex == NULL) {
        ESP_LOGE(TAG, "frame mutex allocation failed");
        return ESP_ERR_NO_MEM;
    }
    if (s_preview_exit_sem == NULL) {
        s_preview_exit_sem = xSemaphoreCreateBinary();
    }
    if (s_preview_exit_sem == NULL) {
        ESP_LOGE(TAG, "preview exit semaphore allocation failed");
        return ESP_ERR_NO_MEM;
    }

    ++s_preview_generation;
    s_preview_running = false;
    if (preview_task_wait_for_exit() != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }

    clear_frame_buffer();
    if (s_canvas_buf != NULL) {
        memset(s_canvas_buf, 0, CAM_PREVIEW_BYTES);
    }
    s_canvas = NULL;
    s_face_box = NULL;
    s_status_label = NULL;
    s_action_button = NULL;
    s_action_button_label = NULL;
    s_last_ui_state = FACE_FOLLOW_STATE_DISABLED;
    s_has_last_ui_state = false;
    s_last_refresh_ms = 0;
    s_received_frame_count = 0;
    s_rendered_frame_count = 0;

    esp_err_t ret = build_page();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "build page failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Keep the preview alive even when the user temporarily stops following. */
    app_activity_camera_acquire(APP_ACT_CAMERA_PAGE);
    s_preview_running = true;
    const uint32_t generation = ++s_preview_generation;
    s_preview_task_generation = generation;
    ret = app_camera_register_frame_cb(
        on_frame, (void *)(uintptr_t)generation);
    if (ret != ESP_OK) {
        s_preview_running = false;
        clear_frame_buffer();
        app_activity_camera_release(APP_ACT_CAMERA_PAGE);
        s_preview_task_generation = 0;
        ESP_LOGE(TAG, "register preview callback failed: %s",
                 esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "preview callback registered generation=%u camera_running=%d",
             (unsigned)generation, app_camera_is_running() ? 1 : 0);

    /* Same priority as the camera page preview: above the background AFE
     * fetch task so the visible image is not delayed by mic processing. */
    BaseType_t task_ret = xTaskCreatePinnedToCoreWithCaps(
        preview_task, "face_prev", PREVIEW_TASK_STACK,
        (void *)(uintptr_t)generation, 6,
        &s_preview_task, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (task_ret != pdPASS) {
        s_preview_running = false;
        (void)app_camera_unregister_frame_cb(on_frame);
        clear_frame_buffer();
        app_activity_camera_release(APP_ACT_CAMERA_PAGE);
        s_preview_task = NULL;
        s_preview_task_generation = 0;
        ESP_LOGE(TAG, "create preview task failed");
        return ESP_ERR_NO_MEM;
    }

    ret = app_face_follow_start();
    ESP_LOGI(TAG, "auto-start face follow: %s preview_generation=%u",
             esp_err_to_name(ret), (unsigned)generation);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "face follow is unavailable; preview remains visible");
    }
    refresh_page_state();
    return ESP_OK;
}

esp_err_t page_face_follow_on_exit(void)
{
    hide_background_popup();
    if (app_face_follow_is_background_enabled()) {
        ESP_LOGI(TAG, "exit page: keeping background face follow enabled");
    } else {
        ESP_LOGI(TAG, "exit page: stopping follow and parking vehicle");
        esp_err_t ret = app_face_follow_stop();
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "stop face follow on exit failed: %s",
                     esp_err_to_name(ret));
        }
        motion_manager_stop_all();
    }

    ++s_preview_generation;
    s_preview_running = false;
    const esp_err_t callback_ret = app_camera_unregister_frame_cb(on_frame);
    if (callback_ret != ESP_OK && callback_ret != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "unregister preview callback failed: %s",
                 esp_err_to_name(callback_ret));
    }
    const esp_err_t preview_ret = preview_task_wait_for_exit();
    if (preview_ret != ESP_OK) {
        ESP_LOGW(TAG, "preview cleanup incomplete: %s",
                 esp_err_to_name(preview_ret));
    }
    const uint32_t stopped_frame_seq = s_frame_seq;
    clear_frame_buffer();
    /* The preview task is joined before the page root is cleaned, and its
     * generation check also rejects any callback snapshot from the old page. */
    s_canvas = NULL;
    s_face_box = NULL;
    s_status_label = NULL;
    s_action_button = NULL;
    s_action_button_label = NULL;
    if (preview_ret == ESP_OK) {
        s_preview_task_generation = 0;
    }
    app_activity_camera_release(APP_ACT_CAMERA_PAGE);
    ESP_LOGI(TAG, "preview stopped: received=%u rendered=%u frame_seq=%u",
             (unsigned)s_received_frame_count,
             (unsigned)s_rendered_frame_count,
             (unsigned)stopped_frame_seq);
    return ESP_OK;
}

void page_face_follow_on_tick(uint32_t ms)
{
    if (ms - s_last_refresh_ms < PAGE_REFRESH_MS) {
        return;
    }
    s_last_refresh_ms = ms;
    refresh_page_state();
}

const ui_page_t page_face_follow = {
    .id = PAGE_FACE_FOLLOW,
    .name = "face_follow",
    .on_enter = page_face_follow_on_enter,
    .on_exit = page_face_follow_on_exit,
    .on_tick = page_face_follow_on_tick,
};
