/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Decode indexed MJPEG samples from a local MP4 or AVI and submit complete
 * RGB565 frames to the display owner. Container parsing stays in the demux
 * modules so this task only deals with decoding, scaling, and timing.
 */
#include "app_video_player.h"

#include "app_avi_demux.h"
#include "app_mp4_demux.h"
#include "esp_xiaozhi_chat_display.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_video_codec_utils.h"
#include "esp_video_dec.h"
#include "esp_video_dec_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <inttypes.h>
#include <stdint.h>
#include <string.h>

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

#define TAG "APP_VIDEO"

#define VIDEO_TASK_STACK_SIZE       (12 * 1024)
#define VIDEO_TASK_PRIORITY         4
#define VIDEO_TASK_CORE             1
#define VIDEO_MAX_ENCODED_BYTES    (512U * 1024U)
#define VIDEO_STOP_WAIT_MS          3000

#define VIDEO_NOTIFY_STOP           (1U << 0)
#define VIDEO_NOTIFY_RESUME         (1U << 1)
#define VIDEO_NOTIFY_OVERLAY        (1U << 2)

typedef struct {
    SemaphoreHandle_t lock;
    SemaphoreHandle_t done;
    TaskHandle_t task;
    bool initialized;
    bool decoder_registered;
    volatile bool stop_requested;
    volatile bool paused;
    volatile int64_t pause_started_us;
    volatile app_video_player_state_t state;
    volatile esp_err_t last_error;
    char path[APP_VIDEO_PLAYER_PATH_MAX];
} video_player_context_t;

static video_player_context_t s_player = {
    .state = APP_VIDEO_PLAYER_STOPPED,
    .last_error = ESP_OK,
};

typedef enum {
    PLAYER_SOURCE_MP4 = 0,
    PLAYER_SOURCE_AVI,
} player_source_kind_t;

typedef struct {
    player_source_kind_t kind;
    app_mp4_demux_t *mp4;
    app_avi_demux_t *avi;
    uint32_t width;
    uint32_t height;
    uint32_t sample_count;
} player_video_source_t;

typedef struct {
    uint32_t size;
    uint64_t pts_ms;
} player_sample_t;

static bool player_stop_requested(void)
{
    return s_player.stop_requested;
}

static void player_set_error(esp_err_t error)
{
    s_player.last_error = error;
}

static esp_err_t player_present_last_frame(const uint16_t *base,
                                           uint16_t *composed,
                                           int width, int height)
{
    esp_err_t ret = app_video_overlay_compose(
        base, composed, width, height,
        (uint32_t)(esp_timer_get_time() / 1000));
    if (ret != ESP_OK) {
        return ret;
    }
    return esp_xiaozhi_chat_display_present_video_frame(
        composed, width, height);
}

static bool player_wait_until(int64_t *timeline_origin_us, uint64_t pts_ms,
                              const uint16_t *base, uint16_t *composed,
                              int width, int height, esp_err_t *out_error)
{
    if (out_error != NULL) {
        *out_error = ESP_OK;
    }
    int64_t pause_start_us = 0;
    const uint32_t notify_mask = VIDEO_NOTIFY_STOP |
                                 VIDEO_NOTIFY_RESUME |
                                 VIDEO_NOTIFY_OVERLAY;

    while (!player_stop_requested()) {
        if (s_player.paused && pause_start_us == 0) {
            pause_start_us = s_player.pause_started_us;
            if (pause_start_us <= 0) {
                pause_start_us = esp_timer_get_time();
            }
        }

        if (!s_player.paused && pause_start_us == 0 &&
            s_player.pause_started_us > 0) {
            pause_start_us = s_player.pause_started_us;
        }

        if (!s_player.paused && pause_start_us > 0) {
            *timeline_origin_us += esp_timer_get_time() - pause_start_us;
            pause_start_us = 0;
            s_player.pause_started_us = 0;
        }

        if (s_player.paused) {
            uint32_t notify_value = 0;
            (void)xTaskNotifyWait(0, notify_mask, &notify_value,
                                  portMAX_DELAY);
            if ((notify_value & VIDEO_NOTIFY_STOP) != 0) {
                return false;
            }
            if ((notify_value & VIDEO_NOTIFY_OVERLAY) != 0 &&
                base != NULL && composed != NULL) {
                esp_err_t ret = player_present_last_frame(
                    base, composed, width, height);
                if (ret != ESP_OK) {
                    if (out_error != NULL) {
                        *out_error = ret;
                    }
                    return false;
                }
            }
            continue;
        }

        const int64_t target_us = *timeline_origin_us +
                                   (int64_t)(pts_ms * 1000ULL);
        const int64_t remaining_us = target_us - esp_timer_get_time();
        if (remaining_us <= 0) {
            return true;
        }
        TickType_t delay_ticks = pdMS_TO_TICKS(
            (uint32_t)(remaining_us / 1000));
        if (delay_ticks == 0) {
            delay_ticks = 1;
        }
        uint32_t notify_value = 0;
        if (xTaskNotifyWait(0, notify_mask, &notify_value,
                            delay_ticks) == pdTRUE) {
            if ((notify_value & VIDEO_NOTIFY_STOP) != 0) {
                return false;
            }
            if ((notify_value & VIDEO_NOTIFY_OVERLAY) != 0 &&
                base != NULL && composed != NULL) {
                esp_err_t ret = player_present_last_frame(
                    base, composed, width, height);
                if (ret != ESP_OK) {
                    if (out_error != NULL) {
                        *out_error = ret;
                    }
                    return false;
                }
            }
        } else {
            return true;
        }
    }
    return false;
}

static bool player_path_ends_with_ignore_case(const char *path,
                                              const char *suffix)
{
    if (path == NULL || suffix == NULL) {
        return false;
    }
    const size_t path_length = strlen(path);
    const size_t suffix_length = strlen(suffix);
    if (path_length < suffix_length) {
        return false;
    }
    path += path_length - suffix_length;
    for (size_t i = 0; i < suffix_length; ++i) {
        char path_char = path[i];
        char suffix_char = suffix[i];
        if (path_char >= 'A' && path_char <= 'Z') {
            path_char = (char)(path_char + ('a' - 'A'));
        }
        if (suffix_char >= 'A' && suffix_char <= 'Z') {
            suffix_char = (char)(suffix_char + ('a' - 'A'));
        }
        if (path_char != suffix_char) {
            return false;
        }
    }
    return true;
}

static bool player_path_is_avi(const char *path)
{
    return player_path_ends_with_ignore_case(path, ".avi") ||
           player_path_ends_with_ignore_case(path, ".mjpeg") ||
           player_path_ends_with_ignore_case(path, ".mjpg");
}

static esp_err_t player_source_get_sample(const player_video_source_t *source,
                                          uint32_t index,
                                          player_sample_t *out_sample)
{
    if (source == NULL || out_sample == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (source->kind == PLAYER_SOURCE_AVI) {
        app_avi_sample_t sample = {0};
        if (app_avi_demux_get_video_sample(source->avi, index, &sample) !=
            APP_AVI_DEMUX_OK) {
            return ESP_ERR_INVALID_SIZE;
        }
        out_sample->size = sample.size;
        out_sample->pts_ms = sample.pts_ms;
        return ESP_OK;
    }

    app_mp4_sample_t sample = {0};
    if (app_mp4_demux_get_video_sample(source->mp4, index, &sample) !=
        APP_MP4_DEMUX_OK) {
        return ESP_ERR_INVALID_SIZE;
    }
    out_sample->size = sample.size;
    out_sample->pts_ms = sample.pts_ms;
    return ESP_OK;
}

static esp_err_t player_source_get_sample_limit(
    const player_video_source_t *source, uint32_t *out_max_size)
{
    if (source == NULL || out_max_size == NULL || source->sample_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t max_size = 0;
    for (uint32_t i = 0; i < source->sample_count; ++i) {
        player_sample_t sample = {0};
        if (player_source_get_sample(source, i, &sample) != ESP_OK) {
            return ESP_ERR_INVALID_SIZE;
        }
        if (sample.size > VIDEO_MAX_ENCODED_BYTES) {
            ESP_LOGE(TAG, "encoded frame %" PRIu32 " exceeds %u bytes",
                     sample.size, VIDEO_MAX_ENCODED_BYTES);
            return ESP_ERR_INVALID_SIZE;
        }
        if (sample.size > max_size) {
            max_size = sample.size;
        }
    }
    *out_max_size = max_size;
    return max_size > 0 ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t player_source_read_sample(player_video_source_t *source,
                                           uint32_t index, uint8_t *buffer,
                                           size_t buffer_size,
                                           player_sample_t *out_sample)
{
    if (source == NULL || buffer == NULL || out_sample == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (source->kind == PLAYER_SOURCE_AVI) {
        app_avi_sample_t sample = {0};
        if (app_avi_demux_read_video_sample(
                source->avi, index, buffer, buffer_size, &sample) !=
            APP_AVI_DEMUX_OK) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        out_sample->size = sample.size;
        out_sample->pts_ms = sample.pts_ms;
        return ESP_OK;
    }

    app_mp4_sample_t sample = {0};
    if (app_mp4_demux_read_video_sample(
            source->mp4, index, buffer, buffer_size, &sample) !=
        APP_MP4_DEMUX_OK) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    out_sample->size = sample.size;
    out_sample->pts_ms = sample.pts_ms;
    return ESP_OK;
}

static void player_source_close(player_video_source_t *source)
{
    if (source == NULL) {
        return;
    }
    app_mp4_demux_close(source->mp4);
    app_avi_demux_close(source->avi);
    source->mp4 = NULL;
    source->avi = NULL;
}

static esp_err_t player_resize_output(esp_video_dec_handle_t decoder,
                                       uint8_t output_alignment,
                                       uint8_t **output_buffer,
                                       uint32_t *output_capacity,
                                       uint32_t required_size)
{
    if (decoder == NULL || output_buffer == NULL || output_capacity == NULL ||
        required_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (*output_buffer != NULL && *output_capacity >= required_size) {
        return ESP_OK;
    }

    uint32_t actual_size = required_size;
    uint8_t *new_buffer = esp_video_codec_align_alloc(
        output_alignment == 0 ? 1 : output_alignment,
        required_size, &actual_size);
    if (new_buffer == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_video_codec_free(*output_buffer);
    *output_buffer = new_buffer;
    *output_capacity = actual_size;
    return ESP_OK;
}

static esp_err_t player_decode_frame(esp_video_dec_handle_t decoder,
                                     uint8_t *encoded, uint32_t encoded_size,
                                     uint64_t pts_ms, uint8_t **output_buffer,
                                     uint32_t *output_capacity,
                                     uint8_t output_alignment,
                                     uint32_t *out_decoded_size,
                                     esp_video_codec_resolution_t *out_resolution)
{
    if (decoder == NULL || encoded == NULL || encoded_size == 0 ||
        output_buffer == NULL || output_capacity == NULL ||
        out_decoded_size == NULL || out_resolution == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint32_t decoder_pts = pts_ms > UINT32_MAX
        ? UINT32_MAX : (uint32_t)pts_ms;
    for (int attempt = 0; attempt < 2; ++attempt) {
        esp_video_dec_in_frame_t input = {
            .pts = decoder_pts,
            .dts = decoder_pts,
            .data = encoded,
            .size = encoded_size,
        };
        esp_video_dec_out_frame_t output = {
            .data = *output_buffer,
            .size = *output_capacity,
        };
        esp_vc_err_t ret = esp_video_dec_process(decoder, &input, &output);
        if (ret == ESP_VC_ERR_BUF_NOT_ENOUGH) {
            esp_video_codec_frame_info_t frame_info = {0};
            (void)esp_video_dec_get_frame_info(decoder, &frame_info);
            uint32_t required_size = output.decoded_size;
            if (frame_info.res.width > 0 && frame_info.res.height > 0) {
                esp_video_codec_resolution_t resolution = frame_info.res;
                required_size = esp_video_codec_get_image_size(
                    ESP_VIDEO_CODEC_PIXEL_FMT_RGB565_LE, &resolution);
            }
            if (required_size == 0 || player_resize_output(
                    decoder, output_alignment, output_buffer,
                    output_capacity, required_size) != ESP_OK) {
                return ESP_ERR_NO_MEM;
            }
            continue;
        }
        if (ret != ESP_VC_ERR_OK || output.decoded_size == 0) {
            ESP_LOGE(TAG, "MJPEG decode failed: %d", ret);
            return ESP_ERR_INVALID_RESPONSE;
        }

        esp_video_codec_frame_info_t frame_info = {0};
        if (esp_video_dec_get_frame_info(decoder, &frame_info) != ESP_VC_ERR_OK ||
            frame_info.res.width == 0 || frame_info.res.height == 0) {
            return ESP_ERR_INVALID_SIZE;
        }
        *out_decoded_size = output.decoded_size;
        *out_resolution = frame_info.res;
        return ESP_OK;
    }
    return ESP_ERR_INVALID_SIZE;
}

static esp_err_t player_scale_rgb565(const uint16_t *source,
                                     uint32_t source_width,
                                     uint32_t source_height,
                                     uint16_t *destination,
                                     uint32_t destination_width,
                                     uint32_t destination_height,
                                     uint32_t destination_stride,
                                     uint32_t destination_x,
                                     uint32_t destination_y)
{
    if (source == NULL || destination == NULL || source_width == 0 ||
        source_height == 0 || destination_width == 0 ||
        destination_height == 0 || destination_stride < destination_width) {
        return ESP_ERR_INVALID_ARG;
    }
    if (destination_x > destination_stride - destination_width) {
        return ESP_ERR_INVALID_ARG;
    }
    for (uint32_t y = 0; y < destination_height; ++y) {
        const uint32_t source_y = (uint32_t)((uint64_t)y * source_height /
                                             destination_height);
        for (uint32_t x = 0; x < destination_width; ++x) {
            const uint32_t source_x = (uint32_t)((uint64_t)x * source_width /
                                                 destination_width);
            destination[(size_t)(y + destination_y) * destination_stride +
                        destination_x + x] =
                source[(size_t)source_y * source_width + source_x];
        }
    }
    return ESP_OK;
}

static void video_player_task(void *arg)
{
    (void)arg;

    char path[APP_VIDEO_PLAYER_PATH_MAX];
    xSemaphoreTake(s_player.lock, portMAX_DELAY);
    strncpy(path, s_player.path, sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';
    xSemaphoreGive(s_player.lock);

    esp_err_t result = ESP_OK;
    player_video_source_t source = {
        .kind = player_path_is_avi(path) ? PLAYER_SOURCE_AVI :
               PLAYER_SOURCE_MP4,
    };
    esp_video_dec_handle_t decoder = NULL;
    uint8_t *encoded_buffer = NULL;
    uint8_t *output_buffer = NULL;
    uint8_t *display_buffer = NULL;
    uint16_t *composed_buffer = NULL;
    uint32_t output_capacity = 0;
    uint8_t output_alignment = 1;

    do {
        int display_width = 0;
        int display_height = 0;
        if (esp_xiaozhi_chat_display_get_width(&display_width) != ESP_OK ||
            esp_xiaozhi_chat_display_get_height(&display_height) != ESP_OK ||
            display_width <= 0 || display_height <= 0) {
            result = ESP_ERR_INVALID_STATE;
            break;
        }
        const uint64_t display_pixels = (uint64_t)display_width *
                                        (uint32_t)display_height;
        if (display_pixels > UINT32_MAX / sizeof(uint16_t)) {
            result = ESP_ERR_INVALID_SIZE;
            break;
        }
        const uint32_t display_frame_size =
            (uint32_t)display_pixels * sizeof(uint16_t);
        display_buffer = heap_caps_malloc(
            display_frame_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        composed_buffer = heap_caps_malloc(
            display_frame_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (display_buffer == NULL || composed_buffer == NULL) {
            result = ESP_ERR_NO_MEM;
            break;
        }
        memset(display_buffer, 0, display_frame_size);

        uint32_t source_width = 0;
        uint32_t source_height = 0;
        if (source.kind == PLAYER_SOURCE_AVI) {
            app_avi_demux_err_t demux_ret = app_avi_demux_open(
                path, &source.avi);
            if (demux_ret != APP_AVI_DEMUX_OK) {
                ESP_LOGE(TAG, "open AVI '%s' failed: %d", path, demux_ret);
                result = ESP_ERR_NOT_FOUND;
                break;
            }
            const app_avi_video_info_t *video =
                app_avi_demux_video_info(source.avi);
            if (video == NULL || video->codec != APP_AVI_VIDEO_CODEC_MJPEG) {
                ESP_LOGE(TAG, "AVI '%s' has no MJPEG video", path);
                result = ESP_ERR_NOT_SUPPORTED;
                break;
            }
            source_width = video->width;
            source_height = video->height;
            source.sample_count = video->sample_count;
        } else {
            app_mp4_demux_err_t demux_ret = app_mp4_demux_open(
                path, &source.mp4);
            if (demux_ret != APP_MP4_DEMUX_OK) {
                ESP_LOGE(TAG, "open MP4 '%s' failed: %d", path, demux_ret);
                result = ESP_ERR_NOT_FOUND;
                break;
            }
            const app_mp4_video_info_t *video =
                app_mp4_demux_video_info(source.mp4);
            if (video == NULL || video->codec != APP_MP4_VIDEO_CODEC_MJPEG) {
                ESP_LOGE(TAG, "MP4 '%s' has no MJPEG video", path);
                result = ESP_ERR_NOT_SUPPORTED;
                break;
            }
            source_width = video->width;
            source_height = video->height;
            source.sample_count = video->sample_count;
        }
        if (source_width == 0 || source_height == 0 ||
            source.sample_count == 0) {
            ESP_LOGE(TAG, "video '%s' has invalid dimensions or no frames",
                     path);
            result = ESP_ERR_NOT_SUPPORTED;
            break;
        }
        source.width = source_width;
        source.height = source_height;
        ESP_LOGI(TAG, "playing %s '%s': %" PRIu32 "x%" PRIu32
                 " -> %dx%d, samples=%" PRIu32,
                 source.kind == PLAYER_SOURCE_AVI ? "AVI" : "MP4", path,
                 source.width, source.height, display_width, display_height,
                 source.sample_count);

        uint32_t encoded_size = 0;
        result = player_source_get_sample_limit(&source, &encoded_size);
        if (result != ESP_OK) {
            break;
        }
        uint8_t input_alignment = 1;
        esp_video_dec_cfg_t decoder_config = {
            .codec_type = ESP_VIDEO_CODEC_TYPE_MJPEG,
            .out_fmt = ESP_VIDEO_CODEC_PIXEL_FMT_RGB565_LE,
        };
        if (esp_video_dec_open(&decoder_config, &decoder) != ESP_VC_ERR_OK) {
            result = ESP_ERR_NOT_SUPPORTED;
            break;
        }
        if (esp_video_dec_get_frame_align(decoder, &input_alignment,
                                          &output_alignment) != ESP_VC_ERR_OK) {
            result = ESP_ERR_INVALID_STATE;
            break;
        }

        uint32_t encoded_alloc_size = encoded_size;
        encoded_buffer = esp_video_codec_align_alloc(
            input_alignment == 0 ? 1 : input_alignment,
            encoded_size, &encoded_alloc_size);
        if (encoded_buffer == NULL) {
            result = ESP_ERR_NO_MEM;
            break;
        }
        output_buffer = esp_video_codec_align_alloc(
            output_alignment == 0 ? 1 : output_alignment,
            display_frame_size, &output_capacity);
        if (output_buffer == NULL) {
            result = ESP_ERR_NO_MEM;
            break;
        }

        int64_t timeline_origin_us = esp_timer_get_time();
        bool first_sample = true;
        for (uint32_t i = 0; i < source.sample_count; ++i) {
            if (player_stop_requested()) {
                break;
            }
            player_sample_t sample = {0};
            result = player_source_read_sample(
                &source, i, encoded_buffer, encoded_alloc_size, &sample);
            if (result != ESP_OK) {
                break;
            }
            if (first_sample) {
                timeline_origin_us = esp_timer_get_time() -
                                     (int64_t)(sample.pts_ms * 1000ULL);
                first_sample = false;
            }
            uint32_t decoded_size = 0;
            esp_video_codec_resolution_t resolution = {0};
            result = player_decode_frame(
                decoder, encoded_buffer, sample.size, sample.pts_ms,
                &output_buffer, &output_capacity, output_alignment,
                &decoded_size, &resolution);
            if (result != ESP_OK) {
                break;
            }
            const uint64_t decoded_pixels = (uint64_t)resolution.width *
                                            resolution.height;
            if (decoded_pixels == 0 || decoded_pixels > UINT32_MAX / 2U ||
                decoded_size < (uint32_t)decoded_pixels * 2U) {
                ESP_LOGE(TAG, "decoded frame has invalid size %" PRIu32
                         "x%" PRIu32 " (%" PRIu32 " bytes)",
                         resolution.width, resolution.height, decoded_size);
                result = ESP_ERR_INVALID_SIZE;
                break;
            }
            esp_err_t wait_error = ESP_OK;
            if (!player_wait_until(&timeline_origin_us, sample.pts_ms,
                                   (const uint16_t *)display_buffer,
                                   composed_buffer, display_width,
                                   display_height, &wait_error)) {
                if (wait_error != ESP_OK) {
                    result = wait_error;
                }
                break;
            }
            memset(display_buffer, 0, display_frame_size);
            const uint32_t fit_width = MIN((uint32_t)display_width,
                (uint32_t)((uint64_t)display_height * resolution.width /
                           resolution.height));
            const uint32_t fit_height = MIN((uint32_t)display_height,
                (uint32_t)((uint64_t)display_width * resolution.height /
                           resolution.width));
            const uint32_t draw_x = ((uint32_t)display_width - fit_width) / 2U;
            const uint32_t draw_y = ((uint32_t)display_height - fit_height) / 2U;
            result = player_scale_rgb565(
                (const uint16_t *)output_buffer, resolution.width,
                resolution.height, (uint16_t *)display_buffer, fit_width,
                fit_height, (uint32_t)display_width, draw_x, draw_y);
            if (result != ESP_OK) {
                break;
            }
            result = player_present_last_frame(
                (const uint16_t *)display_buffer, composed_buffer,
                display_width, display_height);
            if (result != ESP_OK) {
                break;
            }
        }
    } while (0);

    esp_video_dec_close(decoder);
    esp_video_codec_free(encoded_buffer);
    esp_video_codec_free(output_buffer);
    heap_caps_free(display_buffer);
    heap_caps_free(composed_buffer);
    player_source_close(&source);

    xSemaphoreTake(s_player.lock, portMAX_DELAY);
    if (result != ESP_OK && !player_stop_requested()) {
        player_set_error(result);
    } else if (result == ESP_OK) {
        player_set_error(ESP_OK);
    }
    s_player.state = APP_VIDEO_PLAYER_STOPPED;
    s_player.task = NULL;
    xSemaphoreGive(s_player.lock);
    xSemaphoreGive(s_player.done);
    vTaskDelete(NULL);
}

esp_err_t app_video_player_init(void)
{
    if (s_player.initialized) {
        return ESP_OK;
    }
    s_player.lock = xSemaphoreCreateMutex();
    s_player.done = xSemaphoreCreateBinary();
    if (s_player.lock == NULL || s_player.done == NULL) {
        if (s_player.lock != NULL) {
            vSemaphoreDelete(s_player.lock);
            s_player.lock = NULL;
        }
        if (s_player.done != NULL) {
            vSemaphoreDelete(s_player.done);
            s_player.done = NULL;
        }
        return ESP_ERR_NO_MEM;
    }

    esp_vc_err_t ret = esp_video_dec_register_default();
    if (ret != ESP_VC_ERR_OK) {
        ESP_LOGE(TAG, "register video decoders failed: %d", ret);
        vSemaphoreDelete(s_player.lock);
        vSemaphoreDelete(s_player.done);
        s_player.lock = NULL;
        s_player.done = NULL;
        return ESP_ERR_NOT_SUPPORTED;
    }
    s_player.decoder_registered = true;
    s_player.initialized = true;
    return ESP_OK;
}

esp_err_t app_video_player_stop(void)
{
    if (!s_player.initialized) {
        return ESP_OK;
    }

    TaskHandle_t task = NULL;
    xSemaphoreTake(s_player.lock, portMAX_DELAY);
    task = s_player.task;
    s_player.stop_requested = true;
    xSemaphoreGive(s_player.lock);

    if (task != NULL && task != xTaskGetCurrentTaskHandle()) {
        xTaskNotify(task, VIDEO_NOTIFY_STOP, eSetBits);
        if (xSemaphoreTake(s_player.done, pdMS_TO_TICKS(VIDEO_STOP_WAIT_MS)) !=
            pdTRUE) {
            ESP_LOGW(TAG, "video task did not stop within %d ms",
                     VIDEO_STOP_WAIT_MS);
            return ESP_ERR_TIMEOUT;
        }
    }
    return ESP_OK;
}

esp_err_t app_video_player_play(const char *path)
{
    if (path == NULL || path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(path) >= APP_VIDEO_PLAYER_PATH_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t ret = app_video_player_init();
    if (ret != ESP_OK) {
        return ret;
    }
    ret = app_video_player_stop();
    if (ret != ESP_OK) {
        return ret;
    }
    while (xSemaphoreTake(s_player.done, 0) == pdTRUE) {
    }

    xSemaphoreTake(s_player.lock, portMAX_DELAY);
    strncpy(s_player.path, path, sizeof(s_player.path) - 1);
    s_player.path[sizeof(s_player.path) - 1] = '\0';
    s_player.stop_requested = false;
    s_player.paused = false;
    s_player.pause_started_us = 0;
    s_player.state = APP_VIDEO_PLAYER_PLAYING;
    s_player.last_error = ESP_OK;
    BaseType_t task_ret = xTaskCreatePinnedToCoreWithCaps(
        video_player_task, "video_player", VIDEO_TASK_STACK_SIZE, NULL,
        VIDEO_TASK_PRIORITY, &s_player.task, VIDEO_TASK_CORE,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    xSemaphoreGive(s_player.lock);
    if (task_ret != pdPASS) {
        xSemaphoreTake(s_player.lock, portMAX_DELAY);
        s_player.task = NULL;
        s_player.state = APP_VIDEO_PLAYER_STOPPED;
        s_player.last_error = ESP_ERR_NO_MEM;
        xSemaphoreGive(s_player.lock);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t app_video_player_pause(void)
{
    if (!s_player.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_player.lock, portMAX_DELAY);
    if (s_player.task == NULL) {
        xSemaphoreGive(s_player.lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_player.paused) {
        s_player.pause_started_us = esp_timer_get_time();
    }
    s_player.paused = true;
    s_player.state = APP_VIDEO_PLAYER_PAUSED;
    xSemaphoreGive(s_player.lock);
    return ESP_OK;
}

esp_err_t app_video_player_resume(void)
{
    if (!s_player.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_player.lock, portMAX_DELAY);
    TaskHandle_t task = s_player.task;
    if (task == NULL) {
        xSemaphoreGive(s_player.lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_player.paused = false;
    s_player.state = APP_VIDEO_PLAYER_PLAYING;
    xSemaphoreGive(s_player.lock);
    xTaskNotify(task, VIDEO_NOTIFY_RESUME, eSetBits);
    return ESP_OK;
}

esp_err_t app_video_player_set_overlay_state(
    const app_video_overlay_state_t *state)
{
    if (state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    app_video_overlay_set_state(state);
    if (!s_player.initialized) {
        return ESP_OK;
    }

    xSemaphoreTake(s_player.lock, portMAX_DELAY);
    TaskHandle_t task = s_player.task;
    xSemaphoreGive(s_player.lock);
    if (task != NULL && task != xTaskGetCurrentTaskHandle()) {
        xTaskNotify(task, VIDEO_NOTIFY_OVERLAY, eSetBits);
    }
    return ESP_OK;
}

app_video_player_state_t app_video_player_get_state(void)
{
    return s_player.state;
}

esp_err_t app_video_player_last_error(void)
{
    return s_player.last_error;
}
