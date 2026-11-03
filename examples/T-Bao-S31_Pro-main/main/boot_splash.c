/* SPDX-License-Identifier: Apache-2.0 */

#include "boot_splash.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lvgl.h"

#define BOOT_SPLASH_PARTITION_LABEL "welcome"
#define BOOT_SPLASH_GIF_WIDTH      225
#define BOOT_SPLASH_GIF_HEIGHT     65

#ifndef BOOT_SPLASH_GIF_SIZE
#define BOOT_SPLASH_GIF_SIZE 0
#endif

static const char *TAG = "BOOT_SPLASH";

static lv_obj_t *s_root;
static lv_obj_t *s_gif;
static uint8_t *s_gif_data;
static lv_image_dsc_t s_gif_source;
static SemaphoreHandle_t s_finished;
static volatile bool s_playback_finished;

static void boot_splash_release_data(void)
{
    if (s_gif_data != NULL) {
        heap_caps_free(s_gif_data);
        s_gif_data = NULL;
    }
    memset(&s_gif_source, 0, sizeof(s_gif_source));
}

static void boot_splash_ready_cb(lv_event_t *event)
{
    (void)event;
    s_playback_finished = true;
    if (s_finished != NULL) {
        xSemaphoreGive(s_finished);
    }
}

static bool boot_splash_gif_signature_valid(const uint8_t *data, size_t size)
{
    if (data == NULL || size < 6) {
        return false;
    }
    return (memcmp(data, "GIF87a", 6) == 0) ||
           (memcmp(data, "GIF89a", 6) == 0);
}

esp_err_t boot_splash_start(void)
{
    if (s_root != NULL) {
        return ESP_OK;
    }
    if (BOOT_SPLASH_GIF_SIZE == 0) {
        ESP_LOGW(TAG, "boot GIF size is not configured");
        return ESP_ERR_INVALID_STATE;
    }

    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY,
        BOOT_SPLASH_PARTITION_LABEL);
    if (partition == NULL) {
        ESP_LOGW(TAG, "partition '%s' is unavailable",
                 BOOT_SPLASH_PARTITION_LABEL);
        return ESP_ERR_NOT_FOUND;
    }
    if (BOOT_SPLASH_GIF_SIZE > partition->size) {
        ESP_LOGE(TAG, "boot GIF (%u) exceeds partition (%u)",
                 (unsigned)BOOT_SPLASH_GIF_SIZE, (unsigned)partition->size);
        return ESP_ERR_INVALID_SIZE;
    }

    s_gif_data = heap_caps_malloc(BOOT_SPLASH_GIF_SIZE,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_gif_data == NULL) {
        ESP_LOGW(TAG, "cannot allocate %u bytes in PSRAM for boot GIF",
                 (unsigned)BOOT_SPLASH_GIF_SIZE);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = esp_partition_read(partition, 0, s_gif_data,
                                       BOOT_SPLASH_GIF_SIZE);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "cannot read boot GIF: %s", esp_err_to_name(ret));
        boot_splash_release_data();
        return ret;
    }
    if (!boot_splash_gif_signature_valid(s_gif_data, BOOT_SPLASH_GIF_SIZE)) {
        ESP_LOGW(TAG, "boot GIF has an invalid signature");
        boot_splash_release_data();
        return ESP_ERR_INVALID_RESPONSE;
    }

    s_finished = xSemaphoreCreateBinary();
    if (s_finished == NULL) {
        boot_splash_release_data();
        return ESP_ERR_NO_MEM;
    }
    s_playback_finished = false;

    if (!lvgl_port_lock(1000)) {
        vSemaphoreDelete(s_finished);
        s_finished = NULL;
        boot_splash_release_data();
        return ESP_ERR_TIMEOUT;
    }

    s_root = lv_obj_create(lv_screen_active());
    if (s_root == NULL) {
        lvgl_port_unlock();
        vSemaphoreDelete(s_finished);
        s_finished = NULL;
        boot_splash_release_data();
        return ESP_ERR_NO_MEM;
    }
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(s_root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

    s_gif = lv_gif_create(s_root);
    if (s_gif == NULL) {
        lv_obj_delete(s_root);
        s_root = NULL;
        lvgl_port_unlock();
        vSemaphoreDelete(s_finished);
        s_finished = NULL;
        boot_splash_release_data();
        return ESP_ERR_NO_MEM;
    }
    lv_obj_add_event_cb(s_gif, boot_splash_ready_cb, LV_EVENT_READY, NULL);
    lv_gif_set_color_format(s_gif, LV_COLOR_FORMAT_RGB565);

    s_gif_source.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_gif_source.header.cf = LV_COLOR_FORMAT_RAW;
    s_gif_source.header.w = BOOT_SPLASH_GIF_WIDTH;
    s_gif_source.header.h = BOOT_SPLASH_GIF_HEIGHT;
    s_gif_source.data_size = BOOT_SPLASH_GIF_SIZE;
    s_gif_source.data = s_gif_data;
    lv_gif_set_src(s_gif, &s_gif_source);

    if (!lv_gif_is_loaded(s_gif)) {
        lv_obj_delete(s_root);
        s_gif = NULL;
        s_root = NULL;
        lvgl_port_unlock();
        vSemaphoreDelete(s_finished);
        s_finished = NULL;
        boot_splash_release_data();
        ESP_LOGW(TAG, "LVGL could not decode the boot GIF");
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* LVGL's GIF widget keeps its last decoded frame after loop_count == 1. */
    lv_gif_set_loop_count(s_gif, 1);
    lv_obj_align(s_gif, LV_ALIGN_CENTER, 0, 0);
    lv_refr_now(lv_display_get_default());
    lvgl_port_unlock();

    ESP_LOGI(TAG, "boot splash started (%u bytes, one pass)",
             (unsigned)BOOT_SPLASH_GIF_SIZE);
    return ESP_OK;
}

void boot_splash_wait(void)
{
    if (s_root == NULL) {
        return;
    }

    /* The GIF widget emits LV_EVENT_READY after rendering its last frame. The
     * GIF's own frame delays therefore determine when the face page starts. */
    if (!s_playback_finished && s_finished != NULL) {
        (void)xSemaphoreTake(s_finished, portMAX_DELAY);
    }
}

void boot_splash_stop(void)
{
    if (s_root == NULL) {
        return;
    }

    boot_splash_wait();

    if (!lvgl_port_lock(1000)) {
        ESP_LOGW(TAG, "failed to lock LVGL while stopping boot splash");
        return;
    }
    if (s_root != NULL) {
        lv_obj_delete(s_root);
        s_root = NULL;
        s_gif = NULL;
    }
    lvgl_port_unlock();

    if (s_finished != NULL) {
        vSemaphoreDelete(s_finished);
        s_finished = NULL;
    }
    boot_splash_release_data();
    s_playback_finished = false;
}
