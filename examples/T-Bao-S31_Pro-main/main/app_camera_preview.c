/* Decode a 320x320 camera JPEG into RGB565 with the S31 JPEG engine. */
#include "app_camera_preview.h"

#include "driver/jpeg_decode.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/semphr.h"

static const char *TAG = "CAM_PREVIEW";

static SemaphoreHandle_t s_mutex;
static StaticSemaphore_t s_mutex_storage;
static portMUX_TYPE s_mutex_init_lock = portMUX_INITIALIZER_UNLOCKED;
static jpeg_decoder_handle_t s_decoder;
static jpeg_decode_cfg_t s_decode_config;
static bool s_inited;
static esp_err_t s_last_init_error = ESP_OK;
static TickType_t s_retry_after;

static SemaphoreHandle_t get_mutex(void)
{
    /* The preview is shared by the camera page, guard mode, and face follow.
     * Create the lock without consuming heap and publish it atomically. */
    portENTER_CRITICAL(&s_mutex_init_lock);
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_storage);
    }
    SemaphoreHandle_t mutex = s_mutex;
    portEXIT_CRITICAL(&s_mutex_init_lock);
    return mutex;
}

esp_err_t app_camera_preview_init(void)
{
    SemaphoreHandle_t mutex = get_mutex();
    if (mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_inited) {
        xSemaphoreGive(mutex);
        return ESP_OK;
    }

    TickType_t now = xTaskGetTickCount();
    if (s_last_init_error != ESP_OK &&
        (int32_t)(now - s_retry_after) < 0) {
        esp_err_t ret = s_last_init_error;
        xSemaphoreGive(mutex);
        return ret;
    }

    jpeg_decode_engine_cfg_t engine_config = {
        .intr_priority = 0,
        .timeout_ms = 80,
    };
    s_decoder = NULL;
    esp_err_t ret = jpeg_new_decoder_engine(&engine_config, &s_decoder);
    if (ret != ESP_OK) {
        s_decoder = NULL;
        s_last_init_error = ret;
        s_retry_after = now + pdMS_TO_TICKS(200);
        ESP_LOGE(TAG, "create JPEG decoder failed: %s", esp_err_to_name(ret));
        xSemaphoreGive(mutex);
        return ret;
    }
    s_decode_config = (jpeg_decode_cfg_t) {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
    };
    s_inited = true;
    s_last_init_error = ESP_OK;
    s_retry_after = 0;
    ESP_LOGI(TAG, "hardware JPEG decoder ready for %dx%d RGB565", CAM_PREVIEW_W, CAM_PREVIEW_H);
    xSemaphoreGive(mutex);
    return ESP_OK;
}

esp_err_t app_camera_preview_process(const uint8_t *jpeg, size_t jpeg_len, uint8_t *out_rgb565)
{
    if (jpeg == NULL || jpeg_len == 0 || out_rgb565 == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = app_camera_preview_init();
    if (ret != ESP_OK) {
        return ret;
    }

    SemaphoreHandle_t mutex = get_mutex();
    if (mutex == NULL || xSemaphoreTake(mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (!s_inited || s_decoder == NULL) {
        xSemaphoreGive(mutex);
        return ESP_ERR_INVALID_STATE;
    }

    jpeg_decode_picture_info_t picture_info = {0};
    ret = jpeg_decoder_get_info(jpeg, (uint32_t)jpeg_len, &picture_info);
    if (ret == ESP_OK &&
        (picture_info.width != CAM_PREVIEW_W || picture_info.height != CAM_PREVIEW_H)) {
        ret = ESP_ERR_INVALID_SIZE;
    }
    uint32_t output_size = 0;
    if (ret == ESP_OK) {
        ret = jpeg_decoder_process(s_decoder, &s_decode_config, jpeg, (uint32_t)jpeg_len,
                                   out_rgb565, CAM_PREVIEW_BYTES, &output_size);
    }
    if (ret == ESP_OK && output_size != CAM_PREVIEW_BYTES) {
        ret = ESP_ERR_INVALID_SIZE;
    }
    xSemaphoreGive(mutex);
    return ret;
}

void app_camera_preview_deinit(void)
{
    SemaphoreHandle_t mutex = get_mutex();
    if (mutex == NULL || xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    if (s_decoder != NULL) {
        (void)jpeg_del_decoder_engine(s_decoder);
        s_decoder = NULL;
    }
    s_inited = false;
    s_last_init_error = ESP_OK;
    s_retry_after = 0;
    xSemaphoreGive(mutex);
}
