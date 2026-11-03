/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "page_camera.h"

#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "app_activity.h"
#include "app_language.h"
#include "app_camera.h"
#include "app_camera_preview.h"
#include "app_sfx.h"
#include "app_vision_lite.h"
#include "esp_lvgl_port.h"
#include "esp_xiaozhi_chat_display.h"
#include "font_awesome_symbols.h"
#include "http_preview.h"
#include "page_launcher.h"
#include "ui_page_style.h"
#include "launcher_icons.h"

#define TAG "PAGE_CAMERA"

LV_FONT_DECLARE(font_puhui_16_4);

#define PREVIEW_TASK_STACK (12 * 1024)
/* Poll faster than the camera frame interval (~47 ms at 21 fps) so every new
 * JPEG is picked up within a few ms instead of waiting for the next 45 ms
 * slot; an idle poll costs only a queue/sequence check. */
#define PREVIEW_POLL_MS    20

static lv_obj_t *s_canvas;
static lv_color16_t *s_canvas_buf;
static lv_obj_t *s_status;
static lv_obj_t *s_ip_label;

static TaskHandle_t s_preview_task;
static volatile bool s_preview_running;
static volatile bool s_preview_exited;

static bool copy_pending_frame(uint8_t **decode_buf, size_t *decode_capacity,
                               size_t *decode_len, uint32_t *last_seq)
{
    if (decode_buf == NULL || decode_capacity == NULL || decode_len == NULL ||
        last_seq == NULL) {
        return false;
    }
    size_t required = 0;
    uint32_t sequence = 0;
    if (app_camera_get_latest_jpeg_info(&required, &sequence) != ESP_OK) {
        return false;
    }
    if (sequence == *last_seq || required == 0) {
        return false;
    }
    if (*decode_capacity < required) {
        uint8_t *new_buf = heap_caps_realloc(*decode_buf, required,
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (new_buf == NULL) {
            return false;
        }
        *decode_buf = new_buf;
        *decode_capacity = required;
    }
    return app_camera_copy_latest_jpeg(*decode_buf, *decode_capacity,
                                       decode_len, last_seq) == ESP_OK;
}

static void preview_task(void *arg)
{
    (void)arg;
    uint8_t *decode_buf = NULL;
    size_t decode_capacity = 0;
    size_t decode_len = 0;
    uint8_t *output = heap_caps_aligned_alloc(64, CAM_PREVIEW_BYTES,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint32_t last_seq = 0;
    uint32_t frame_number = 0;

    if (output == NULL) {
        ESP_LOGE(TAG, "preview output buffer allocation failed");
        s_preview_exited = true;
        for (;;) {
            vTaskSuspend(NULL);
        }
    }

    while (s_preview_running) {
        if (!copy_pending_frame(&decode_buf, &decode_capacity, &decode_len, &last_seq)) {
            vTaskDelay(pdMS_TO_TICKS(PREVIEW_POLL_MS));
            continue;
        }

        /* Decode outside the LVGL lock: the decoder mutex is shared with the
         * face-follow / guard inference tasks, and waiting for it while
         * holding the LVGL lock would stop the canvas from being refreshed. */
        if (app_camera_preview_process(decode_buf, decode_len, output) == ESP_OK) {
            frame_number++;
            /* Vision analysis is a CPU-heavy full-frame scan; 4-5 fps is
             * plenty for motion/wave detection. */
            if ((frame_number % 5) == 1) {
                app_vision_lite_feed_rgb565(output, CAM_PREVIEW_W, CAM_PREVIEW_H,
                                            (uint32_t)(esp_timer_get_time() / 1000));
            }
            if (s_preview_running && lvgl_port_lock(1000)) {
                if (s_canvas != NULL && s_canvas_buf != NULL) {
                    memcpy(s_canvas_buf, output, CAM_PREVIEW_BYTES);
                    lv_obj_invalidate(s_canvas);
                }
                lvgl_port_unlock();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(PREVIEW_POLL_MS));
    }

    heap_caps_free(decode_buf);
    heap_caps_free(output);
    s_preview_exited = true;
    /* The preview stack was allocated with xTaskCreatePinnedToCoreWithCaps;
     * page_camera_on_exit() reclaims it after this task is suspended. */
    vTaskSuspend(NULL);
}

static void reap_preview_task(void)
{
    TaskHandle_t task = s_preview_task;
    if (task != NULL && s_preview_exited) {
        vTaskSuspend(task);
        vTaskDeleteWithCaps(task);
        s_preview_task = NULL;
    }
}

static void build_preview(void)
{
    lv_obj_t *root = esp_xiaozhi_chat_display_page_root();
    if (root == NULL) {
        return;
    }
    ui_page_style_root(root);

    if (s_canvas_buf == NULL) {
        s_canvas_buf = heap_caps_aligned_alloc(64, CAM_PREVIEW_BYTES,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_canvas_buf == NULL) {
            return;
        }
        memset(s_canvas_buf, 0, CAM_PREVIEW_BYTES);
    }
    s_canvas = lv_canvas_create(root);
    lv_canvas_set_buffer(s_canvas, s_canvas_buf, CAM_PREVIEW_W, CAM_PREVIEW_H,
                         LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(s_canvas, 0, 0);

    ui_page_make_back_button(root);
    ui_page_make_header_image(root, &launcher_icon_camera, "摄像头", "实时预览");

    s_status = lv_label_create(root);
    lv_obj_set_style_text_font(s_status, &font_puhui_16_4, 0);
    lv_obj_set_style_text_color(s_status, lv_color_hex(0xF4F7F8), 0);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_status, LV_HOR_RES - 24);
    lv_obj_align(s_status, LV_ALIGN_BOTTOM_MID, 0, -9);
    lv_label_set_text(s_status, app_language_text("预览启动中...", "Starting preview..."));

    s_ip_label = lv_label_create(root);
    lv_obj_set_style_text_font(s_ip_label, &font_puhui_16_4, 0);
    lv_obj_set_style_text_color(s_ip_label, lv_color_hex(0xFFB26B), 0);
    lv_obj_set_style_text_align(s_ip_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(s_ip_label, LV_HOR_RES - 24);
    lv_obj_align(s_ip_label, LV_ALIGN_TOP_MID, 0, 55);
    lv_label_set_text(s_ip_label, "IP: --");
}

static void refresh_ip_label(void)
{
    if (s_ip_label == NULL) {
        return;
    }
    char ip_str[16] = "0.0.0.0";
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif != NULL) {
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
            esp_ip4addr_ntoa(&ip.ip, ip_str, sizeof(ip_str));
        }
    }
    static char last_ip[16];
    if (strcmp(last_ip, ip_str) != 0) {
        strncpy(last_ip, ip_str, sizeof(last_ip) - 1);
        last_ip[sizeof(last_ip) - 1] = '\0';
        char text[96];
        snprintf(text, sizeof(text), "Web: http://%s/", ip_str);
        lv_label_set_text(s_ip_label, text);
    }
}

esp_err_t page_camera_on_enter(void)
{
    app_sfx_play(APP_SFX_PAGE);

    if (s_preview_task != NULL) {
        lvgl_port_unlock();
        int wait_count = 0;
        while (s_preview_task != NULL && !s_preview_exited && wait_count++ < 150) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        reap_preview_task();
        while (!lvgl_port_lock(1000)) {
            ESP_LOGW(TAG, "waiting for LVGL lock after camera preview stop");
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (s_preview_task != NULL) {
            ESP_LOGW(TAG, "camera preview task did not exit in time");
            return ESP_ERR_TIMEOUT;
        }
    }

    build_preview();
    app_activity_camera_acquire(APP_ACT_CAMERA_PAGE);
    esp_err_t preview_ret = http_preview_start();
    if (preview_ret != ESP_OK) {
        ESP_LOGW(TAG, "camera web preview unavailable: %s",
                 esp_err_to_name(preview_ret));
    }
    if (app_vision_lite_init() != ESP_OK) {
        ESP_LOGW(TAG, "vision analyzer initialization failed");
    }
    app_vision_lite_set_enabled(true);

    s_preview_exited = false;
    s_preview_running = true;
    /* Above the AFE fetch task (prio 5) and the camera capture task (prio 6):
     * the visible preview must win over background mic processing when both
     * are runnable.  The task blocks on the JPEG decoder and the LVGL lock, so
     * it cannot starve audio. */
    BaseType_t task_ret = xTaskCreatePinnedToCoreWithCaps(
        preview_task, "cam_prev", PREVIEW_TASK_STACK, NULL, 6, &s_preview_task, 0,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (task_ret != pdPASS) {
        s_preview_running = false;
        s_preview_exited = true;
        s_preview_task = NULL;
        app_vision_lite_set_enabled(false);
        http_preview_stop();
        app_activity_camera_release(APP_ACT_CAMERA_PAGE);
        return ESP_ERR_NO_MEM;
    }

    refresh_ip_label();
    lv_label_set_text(s_status, app_language_text("预览中 · 浏览器打开上方地址", "Previewing; open the address above in a browser"));
    return ESP_OK;
}

esp_err_t page_camera_on_exit(void)
{
    http_preview_stop();
    app_vision_lite_set_enabled(false);
    s_preview_running = false;

    /* Do not release the camera demand until the preview worker has stopped
     * using the shared JPEG decoder. app_camera_stop() may otherwise tear down
     * the decoder while this task is still inside jpeg_decoder_process(). */
    if (s_preview_task != NULL) {
        lvgl_port_unlock();
        int wait_count = 0;
        while (s_preview_task != NULL && !s_preview_exited) {
            vTaskDelay(pdMS_TO_TICKS(10));
            if (++wait_count % 100 == 0) {
                ESP_LOGW(TAG, "waiting for camera preview task to exit");
            }
        }
        reap_preview_task();
        while (!lvgl_port_lock(1000)) {
            ESP_LOGW(TAG, "waiting for LVGL lock after camera preview stop");
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    s_canvas = NULL;
    s_status = NULL;
    s_ip_label = NULL;
    app_activity_web_release();
    app_activity_camera_release(APP_ACT_CAMERA_PAGE);
    return ESP_OK;
}

void page_camera_on_tick(uint32_t ms)
{
    static uint32_t last_refresh;
    if (ms - last_refresh < 300) {
        return;
    }
    last_refresh = ms;
    refresh_ip_label();

    if (s_status == NULL || !app_vision_lite_is_enabled()) {
        return;
    }
    app_vision_result_t vision = app_vision_lite_get_result();
    const char *activity_text = "-";
    if (vision.activity == VISION_ACTIVITY_NONE) {
        activity_text = app_language_text("无活动", "No activity");
    } else if (vision.activity == VISION_ACTIVITY_QUIET) {
        activity_text = app_language_text("轻微", "Low");
    } else {
        activity_text = app_language_text("活跃", "Active");
    }
    char status_text[96];
    if (app_language_is_english()) {
        snprintf(status_text, sizeof(status_text), "Motion: %d%% %s%s",
                 vision.motion_ratio, activity_text,
                 vision.wave ? " (wave)" : "");
    } else {
        snprintf(status_text, sizeof(status_text), "运动:%d%% %s%s",
                 vision.motion_ratio, activity_text,
                 vision.wave ? "  👋挥手" : "");
    }
    lv_label_set_text(s_status, status_text);
}

const ui_page_t page_camera = {
    .id = PAGE_CAMERA,
    .name = "camera",
    .on_enter = page_camera_on_enter,
    .on_exit = page_camera_on_exit,
    .on_tick = page_camera_on_tick,
};
