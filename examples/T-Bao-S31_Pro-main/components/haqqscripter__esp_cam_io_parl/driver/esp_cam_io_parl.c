#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include "esp_check.h"
#include "esp_cpu.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "portmacro.h"
#include "soc/clk_tree_defs.h"
#include "time.h"

#include "driver/parlio_rx.h"
#include "hal/parlio_types.h"
#include "esp_cam_io_parl.h"

static const char *TAG = "esp_cam_io_parl";

/* The GDMA callback can run on the other core while the application task is
 * disabling and destroying the RX unit. Admission is serialized with the
 * teardown task so the handle cannot be freed while a callback still uses it. */
static portMUX_TYPE s_callback_lock = portMUX_INITIALIZER_UNLOCKED;

#define ESP_CAM_IO_PARL_CHECK_ISR(condition, err) if (!(condition)) { return err; }
#define MIN_FRAME_ALLOC_SIZE 64

#define MIN_STREAM_CHUNK_SIZE 32768
#define MAX_STREAM_CHUNK_SIZE 65535
#define ISR_PAYLOAD_SIZE 2048U

/* ISR staging ring (3 x 2 KB) reserved in .bss.
 *
 * It used to be heap_caps_calloc(MALLOC_CAP_DMA|INTERNAL) at every camera
 * start.  With XiaoZhi and the face-follow model loaded, the internal heap is
 * fragmented enough that no 6 KB contiguous block exists any more (observed:
 * free=19 KB but largest=5.3 KB), so camera start failed with ESP_ERR_NO_MEM
 * and the preview stayed blank.  A static buffer can never fail to allocate. */
static uint8_t s_chunk_storage[ESP_CAM_IO_PARL_CHUNK_COUNT * ISR_PAYLOAD_SIZE];

/* PARLIO RX DMA payload: static internal DRAM (cache-line aligned).  It used
 * to be a 2 KB heap allocation per camera start; with the internal heap
 * fragmented to ~1 KB blocks this also failed.  Static DRAM is DMA-capable and
 * it is the same memory the GDMA would have received from the heap. */
static uint8_t s_payload[ISR_PAYLOAD_SIZE] __attribute__((aligned(64)));

#ifndef CONFIG_CAMERA_PAYLOAD_BUFFER_SIZE
#define CONFIG_CAMERA_PAYLOAD_BUFFER_SIZE 0x8000 // 32768
#endif

static void IRAM_ATTR copy_payload_from_isr(uint8_t *dst, const uint8_t *src, size_t length)
{
    volatile uint8_t *iram_dst = dst;
    const volatile uint8_t *iram_src = src;
    for (size_t i = 0; i < length; ++i) {
        iram_dst[i] = iram_src[i];
    }
}

static bool IRAM_ATTR callback_enter(esp_cam_io_parl_handle_t io)
{
    if (io == NULL) {
        return false;
    }
    bool accepted = false;
    portENTER_CRITICAL_ISR(&s_callback_lock);
    if (__atomic_load_n(&io->callback_accepting, __ATOMIC_ACQUIRE)) {
        __atomic_add_fetch(&io->callback_inflight, 1U, __ATOMIC_ACQ_REL);
        accepted = true;
    }
    portEXIT_CRITICAL_ISR(&s_callback_lock);
    return accepted;
}

static void IRAM_ATTR callback_leave(esp_cam_io_parl_handle_t io)
{
    if (io != NULL) {
        __atomic_sub_fetch(&io->callback_inflight, 1U, __ATOMIC_RELEASE);
    }
}

static void wait_callbacks_idle(esp_cam_io_parl_handle_t io)
{
    while (__atomic_load_n(&io->callback_inflight, __ATOMIC_ACQUIRE) != 0U) {
        vTaskDelay(1);
    }
}

static bool esp_cam_io_parl_received_partial_data(parlio_rx_unit_handle_t rx_unit,
                                                   const parlio_rx_event_data_t *edata,
                                                   void *user_data);

static esp_err_t register_partial_callback(esp_cam_io_parl_handle_t io)
{
    parlio_rx_event_callbacks_t cbs = {
        .on_partial_receive = esp_cam_io_parl_received_partial_data,
    };
    return parlio_rx_unit_register_event_callbacks(io->rx_unit, &cbs, io);
}

static bool IRAM_ATTR esp_cam_io_parl_received_partial_data(parlio_rx_unit_handle_t rx_unit,
                                                             const parlio_rx_event_data_t *edata,
                                                             void *user_data)
{
    (void)rx_unit;
    esp_cam_io_parl_handle_t io = (esp_cam_io_parl_handle_t)user_data;
    if (edata == NULL || !callback_enter(io)) {
        return false;
    }
    bool result = true;
    BaseType_t hp_task_woken = pdFALSE;
    if (__atomic_load_n(&io->cam_task_stop, __ATOMIC_ACQUIRE) ||
        io->chunk_free_queue == NULL || io->chunk_ready_queue == NULL ||
        io->chunk_storage == NULL) {
        result = false;
        goto exit;
    }
    const size_t length = edata->recv_bytes;
    uint8_t slot = 0;

    /* ISR code only touches the internal DMA payload and internal staging
     * chunks. JPEG parsing and PSRAM frame writes happen in cam_task. */
    if (length == 0 || length > io->chunk_size ||
        xQueueReceiveFromISR(io->chunk_free_queue, &slot, &hp_task_woken) != pdTRUE) {
        io->chunk_overflow = true;
        goto exit;
    }
    copy_payload_from_isr(io->chunk_storage + ((size_t)slot * io->chunk_size),
                          edata->data, length);

    esp_cam_io_parl_chunk_t chunk = {
        .slot = slot,
        .length = (uint16_t)length,
    };
    if (xQueueSendFromISR(io->chunk_ready_queue, &chunk, &hp_task_woken) != pdTRUE) {
        (void)xQueueSendFromISR(io->chunk_free_queue, &slot, &hp_task_woken);
        io->chunk_overflow = true;
    }
exit:
    callback_leave(io);
    if (hp_task_woken) {
        portYIELD_FROM_ISR();
    }
    return result;
}

static void reset_capture(esp_cam_io_parl_handle_t io)
{
    uint8_t *capture_buffer = io->info.frame.buffer;
    /* Clear ownership before freeing. If the driver task is stopped during
     * teardown between these operations, the destroy path must not free it
     * a second time. */
    io->info.frame.buffer = NULL;
    if (capture_buffer != NULL) {
        heap_caps_free(capture_buffer);
    }
    memset(&io->info.frame, 0, sizeof(io->info.frame));
    io->info.previous_byte = 0;
    io->info.index = 0;
    io->info.state = ESP_CAM_IO_PARL_JPEG_IDLE;
}

static bool append_capture(esp_cam_io_parl_handle_t io, const uint8_t *data, size_t length)
{
    if (io->info.frame.buffer == NULL || io->info.index + length > io->info.frame.length) {
        return false;
    }
    memcpy(io->info.frame.buffer + io->info.index, data, length);
    io->info.index += length;
    return true;
}

static bool start_capture(esp_cam_io_parl_handle_t io)
{
    if (io->info.frame.buffer != NULL) {
        return true;
    }
    io->info.frame.buffer = heap_caps_malloc(io->alloc_size, io->alloc_heap_caps);
    if (io->info.frame.buffer == NULL) {
        return false;
    }
    io->info.frame.length = io->alloc_size;
    io->info.index = 0;
    io->info.state = ESP_CAM_IO_PARL_JPEG_HEADER;
    return append_capture(io, (const uint8_t[]){0xFF, 0xD8}, 2);
}

static void publish_capture(esp_cam_io_parl_handle_t io)
{
    esp_cam_io_parl_trans_t frame = io->info.frame;
    frame.length = io->info.index;
    /* Transfer ownership before any queue operation. If the task is deleted
     * while handling a full READY queue, the teardown path must not see this
     * buffer through info.frame after it has already been freed below. */
    memset(&io->info.frame, 0, sizeof(io->info.frame));
    io->info.index = 0;
    io->info.state = ESP_CAM_IO_PARL_JPEG_IDLE;
    if (xQueueSend(io->queue_handle[ESP_CAM_IO_PARL_QUEUE_READY], &frame, 0) != pdTRUE) {
        if (io->config.fill_mode == ESP_CAM_IO_PARL_QUEUE_LATEST) {
            esp_cam_io_parl_trans_t old = {0};
            if (xQueueReceive(io->queue_handle[ESP_CAM_IO_PARL_QUEUE_READY], &old, 0) == pdTRUE) {
                heap_caps_free(old.buffer);
            }
            if (xQueueSend(io->queue_handle[ESP_CAM_IO_PARL_QUEUE_READY], &frame, 0) != pdTRUE) {
                heap_caps_free(frame.buffer);
            }
        } else {
            heap_caps_free(frame.buffer);
        }
    }
}

static void process_capture_chunk(esp_cam_io_parl_handle_t io, const uint8_t *chunk, size_t chunk_len)
{
    size_t offset = 0;
    while (offset < chunk_len) {
        if (io->info.state == ESP_CAM_IO_PARL_JPEG_IDLE) {
            if (offset == 0 && io->info.previous_byte == 0xFF && chunk[0] == 0xD8) {
                (void)start_capture(io);
                offset = 1;
                io->info.previous_byte = 0;
                continue;
            }
            while (offset < chunk_len && chunk[offset] != 0xFF) {
                offset++;
            }
            if (offset >= chunk_len) {
                break;
            }
            const bool normal_soi = offset + 1 < chunk_len && chunk[offset + 1] == 0xD8;
            if (normal_soi) {
                (void)start_capture(io);
                offset += 2;
                io->info.previous_byte = 0xFF;
                continue;
            }
            offset++;
            continue;
        }

        /* The previous chunk may have ended immediately after 0xFF. That byte
         * was already copied; interpret the first byte here as its marker type. */
        if (offset == 0 && io->info.previous_byte == 0xFF) {
            const uint8_t marker_type = chunk[0];
            if (marker_type == 0xDA && io->info.state == ESP_CAM_IO_PARL_JPEG_HEADER) {
                if (!append_capture(io, &marker_type, 1)) {
                    reset_capture(io);
                    break;
                }
                io->info.state = ESP_CAM_IO_PARL_JPEG_ENTROPY;
                offset = 1;
                io->info.previous_byte = 0;
                continue;
            }
            if (marker_type == 0xD9 && io->info.state == ESP_CAM_IO_PARL_JPEG_ENTROPY) {
                if (!append_capture(io, &marker_type, 1)) {
                    reset_capture(io);
                    break;
                }
                offset = 1;
                publish_capture(io);
                io->info.previous_byte = 0;
                continue;
            }
            if (!append_capture(io, &marker_type, 1)) {
                reset_capture(io);
                break;
            }
            offset = 1;
            io->info.previous_byte = 0;
            continue;
        }

        const uint8_t *marker = memchr(chunk + offset, 0xFF, chunk_len - offset);
        const size_t bytes = marker != NULL ? (size_t)(marker - (chunk + offset))
                                            : chunk_len - offset;
        if (!append_capture(io, chunk + offset, bytes)) {
            ESP_EARLY_LOGW(TAG, "JPEG buffer overflow");
            reset_capture(io);
            break;
        }
        offset += bytes;
        if (marker == NULL) {
            break;
        }
        if (offset + 1 >= chunk_len) {
            if (!append_capture(io, chunk + offset, 1)) {
                reset_capture(io);
                break;
            }
            io->info.previous_byte = 0xFF;
            break;
        }

        const uint8_t marker_type = chunk[offset + 1];
        if (marker_type == 0xDA && io->info.state == ESP_CAM_IO_PARL_JPEG_HEADER) {
            if (!append_capture(io, chunk + offset, 2)) {
                reset_capture(io);
                break;
            }
            io->info.state = ESP_CAM_IO_PARL_JPEG_ENTROPY;
            offset += 2;
        } else if (marker_type == 0xD9 && io->info.state == ESP_CAM_IO_PARL_JPEG_ENTROPY) {
            if (!append_capture(io, chunk + offset, 2)) {
                reset_capture(io);
                break;
            }
            offset += 2;
            publish_capture(io);
        } else {
            if (!append_capture(io, chunk + offset, 1)) {
                reset_capture(io);
                break;
            }
            offset++;
        }
    }
    if (chunk_len > 0) {
        io->info.previous_byte = chunk[chunk_len - 1];
    }
}

static void esp_cam_io_parl_task(void *user_data)
{
    esp_cam_io_parl_handle_t io = (esp_cam_io_parl_handle_t)user_data;
    while (!__atomic_load_n(&io->cam_task_stop, __ATOMIC_ACQUIRE)) {
        esp_cam_io_parl_chunk_t chunk = {0};
        if (xQueueReceive(io->chunk_ready_queue, &chunk, pdMS_TO_TICKS(20)) == pdTRUE) {
            if (__atomic_load_n(&io->cam_task_stop, __ATOMIC_ACQUIRE)) {
                (void)xQueueSend(io->chunk_free_queue, &chunk.slot, 0);
                break;
            }
            if (io->chunk_overflow) {
                io->chunk_overflow = false;
                reset_capture(io);
            }
            process_capture_chunk(io,
                                  io->chunk_storage + ((size_t)chunk.slot * io->chunk_size),
                                  chunk.length);
            (void)xQueueSend(io->chunk_free_queue, &chunk.slot, portMAX_DELAY);
        } else if (io->chunk_overflow) {
            io->chunk_overflow = false;
            reset_capture(io);
        }
    }
    if (io->cam_task_exit != NULL) {
        (void)xSemaphoreGive(io->cam_task_exit);
    }
    /* The task stack was allocated by xTaskCreateWithCaps(). Keep the task
     * suspended until the owner reclaims it with vTaskDeleteWithCaps(). */
    vTaskSuspend(NULL);
}

static void drain_chunk_queues(esp_cam_io_parl_handle_t io)
{
    xQueueReset(io->chunk_ready_queue);
    xQueueReset(io->chunk_free_queue);
    for (uint8_t slot = 0; slot < ESP_CAM_IO_PARL_CHUNK_COUNT; ++slot) {
        (void)xQueueSend(io->chunk_free_queue, &slot, 0);
    }
    io->chunk_overflow = false;
    /* Do not free info.frame here. The driver task may have been deleted
     * between publishing a frame to READY and clearing info.frame. Keep the
     * pointer for esp_cam_release_frame_buffers(), which de-duplicates it
     * against queued frames before freeing anything. */
}

static void esp_cam_release_frame_buffers(esp_cam_io_parl_handle_t esp_cam_io_parl) {
    uint8_t *capture_buffer = esp_cam_io_parl->info.frame.buffer;
    bool capture_buffer_owned = capture_buffer != NULL &&
                                esp_cam_io_parl->info.state != ESP_CAM_IO_PARL_JPEG_IDLE;

    // The JPEG error path can leave info.frame pointing at the same buffer
    // stored in the fail queue. Clear it before draining queues to prevent a
    // double free while still releasing an in-progress capture.
    esp_cam_io_parl->info.frame.buffer = NULL;
    esp_cam_io_parl->info.frame.length = 0;
    esp_cam_io_parl->info.index = 0;
    esp_cam_io_parl->info.state = ESP_CAM_IO_PARL_JPEG_IDLE;

    for (int i = 0; i < ESP_CAM_IO_PARL_QUEUE_MAX; i++) {
        QueueHandle_t queue = esp_cam_io_parl->queue_handle[i];
        if (queue == NULL) {
            continue;
        }

        esp_cam_io_parl_trans_t frame = {0};
        while (xQueueReceive(queue, &frame, 0) == pdTRUE) {
            if (frame.buffer == NULL) {
                continue;
            }
            if (frame.buffer == capture_buffer) {
                capture_buffer = NULL;
            }
            heap_caps_free(frame.buffer);
        }
    }

    if (capture_buffer_owned && capture_buffer != NULL) {
        heap_caps_free(capture_buffer);
    }
}

static esp_err_t esp_cam_destroy_io_parl(esp_cam_io_parl_handle_t esp_cam_io_parl) {
    esp_cam_release_frame_buffers(esp_cam_io_parl);
    for (int i = 0; i < ESP_CAM_IO_PARL_QUEUE_MAX; i++) {
        if (esp_cam_io_parl->queue_handle[i]) {
            vQueueDeleteWithCaps(esp_cam_io_parl->queue_handle[i]);
        }
    }
    if (esp_cam_io_parl->chunk_free_queue) {
        vQueueDeleteWithCaps(esp_cam_io_parl->chunk_free_queue);
    }
    if (esp_cam_io_parl->chunk_ready_queue) {
        vQueueDeleteWithCaps(esp_cam_io_parl->chunk_ready_queue);
    }
    /* chunk_storage and payload are static .bss buffers; never free them. */
    if (esp_cam_io_parl->rx_delimiter) {
        parlio_del_rx_delimiter(esp_cam_io_parl->rx_delimiter);
    }
    if (esp_cam_io_parl->rx_unit) {
        parlio_del_rx_unit(esp_cam_io_parl->rx_unit);
    }
    heap_caps_free(esp_cam_io_parl);
    return ESP_OK;
}

esp_err_t esp_cam_new_io_parl(const esp_cam_io_parl_config_t *config, esp_cam_io_parl_handle_t *ret_handle) {
    esp_err_t ret = ESP_OK;
    ESP_RETURN_ON_FALSE(config && ret_handle, ESP_ERR_INVALID_ARG, TAG, "Invalid argument");
    *ret_handle = NULL;
    ESP_RETURN_ON_FALSE(__builtin_popcount(config->data_width) == 1, ESP_ERR_INVALID_ARG, TAG, "Data line number should be the power of 2 without counting valid signal");
    esp_cam_io_parl_handle_t esp_cam_io_parl = heap_caps_calloc(1, sizeof(esp_cam_io_parl_t), MALLOC_CAP_INTERNAL);
    ESP_GOTO_ON_FALSE(esp_cam_io_parl, ESP_ERR_NO_MEM, err, TAG, "No memory for allocating rx unit");
    esp_cam_io_parl->cam_task_exit =
        xSemaphoreCreateBinaryStatic(&esp_cam_io_parl->cam_task_exit_storage);
    ESP_GOTO_ON_FALSE(esp_cam_io_parl->cam_task_exit, ESP_ERR_NO_MEM, err, TAG,
                      "No memory for camera task exit semaphore");
    /* Smaller internal chunks bound ISR work and keep the staging ring in
     * internal RAM. The application frame remains PSRAM-backed. */
    esp_cam_io_parl->payload_size = MIN(CONFIG_ESP_CAM_IO_PARL_PAYLOAD_SIZE,
                                        ISR_PAYLOAD_SIZE);
    esp_cam_io_parl->chunk_size = esp_cam_io_parl->payload_size;
    gpio_num_t valid_gpio = config->de_io >= 0 ? config->de_io : (config->hsync_io >= 0 ? config->hsync_io : -1);
    uint32_t pclk_freq = config->pclk_hz > 0 ? config->pclk_hz : (80 * 1000 * 1000);
    parlio_rx_unit_config_t rx_unit_config = {
        .trans_queue_depth = 4,
        .max_recv_size = UINT16_MAX,
        .data_width = config->data_width,
        .clk_src = PARLIO_CLK_SRC_EXTERNAL,
        .ext_clk_freq_hz = pclk_freq,
        .exp_clk_freq_hz = pclk_freq,
        .clk_in_gpio_num = config->pclk_io,
        .clk_out_gpio_num = -1,
        .valid_gpio_num = valid_gpio,
        .flags = {
            .free_clk = false,
            .allow_pd = config->flags.allow_pd,
        },
    };
    memcpy(rx_unit_config.data_gpio_nums, config->data_io, PARLIO_RX_UNIT_MAX_DATA_WIDTH * sizeof(gpio_num_t));
    ESP_GOTO_ON_ERROR(parlio_new_rx_unit(&rx_unit_config, &esp_cam_io_parl->rx_unit), err, TAG, "Failed to initialize rx unit");
    esp_cam_io_parl->config = *config;
    if (esp_cam_io_parl->config.vsync_io >= 0) {
        ESP_LOGD(TAG, "VSYNC signal can not be used, ignoring the assigned pin");
    }
    if (valid_gpio >= 0 && config->data_width <= 8 && PARLIO_RX_UNIT_MAX_DATA_WIDTH > 8) {
        if (config->de_io >= 0) {
            parlio_rx_level_delimiter_config_t rx_delimiter_config = {
                .valid_sig_line_id = PARLIO_RX_UNIT_MAX_DATA_WIDTH - 1,
#if ESP_CAM_IO_PARL_EDGE_FIX
                .sample_edge = config->pclk_sample_edge,
#else
                .sample_edge = (config->pclk_sample_edge == PARLIO_SAMPLE_EDGE_POS) ? PARLIO_SAMPLE_EDGE_NEG : PARLIO_SAMPLE_EDGE_POS,
#endif
                .eof_data_len = MIN(esp_cam_io_parl->payload_size, UINT16_MAX),
                .timeout_ticks = 0,
                .flags = {
                    .active_low_en = config->flags.invert_de,
                },
            };
            ESP_GOTO_ON_ERROR(parlio_new_rx_level_delimiter(&rx_delimiter_config, &esp_cam_io_parl->rx_delimiter), err, TAG, "Failed to initialize rx level delimiter");
        }
        else if (config->hsync_io >= 0) {
            parlio_rx_pulse_delimiter_config_t rx_delimiter_config = {
                .valid_sig_line_id = PARLIO_RX_UNIT_MAX_DATA_WIDTH - 1,
#if ESP_CAM_IO_PARL_EDGE_FIX
                .sample_edge = config->pclk_sample_edge,
#else
                .sample_edge = (config->pclk_sample_edge == PARLIO_SAMPLE_EDGE_POS) ? PARLIO_SAMPLE_EDGE_NEG : PARLIO_SAMPLE_EDGE_POS,
#endif
                .eof_data_len = MIN(esp_cam_io_parl->payload_size, UINT16_MAX),
                .timeout_ticks = 0,
                .flags = {
                    .start_bit_included = false,
                    .end_bit_included = false,
                    .has_end_pulse = true,
                    .pulse_invert = config->flags.invert_hsync,
                },
            };
            ESP_GOTO_ON_ERROR(parlio_new_rx_pulse_delimiter(&rx_delimiter_config, &esp_cam_io_parl->rx_delimiter), err, TAG, "Failed to initialize rx pulse delimiter");
        }
        esp_cam_io_parl->use_soft_delimiter = false;
    }
    else {
        if (valid_gpio >= 0) {
            ESP_LOGD(TAG, "Valid signal can not be used, ignoring the assigned pin");
        }
        parlio_rx_soft_delimiter_config_t rx_delimiter_config = {
#if ESP_CAM_IO_PARL_EDGE_FIX
            .sample_edge = config->pclk_sample_edge,
#else
            .sample_edge = (config->pclk_sample_edge == PARLIO_SAMPLE_EDGE_POS) ? PARLIO_SAMPLE_EDGE_NEG : PARLIO_SAMPLE_EDGE_POS,
#endif
            .eof_data_len = MIN(esp_cam_io_parl->payload_size, UINT16_MAX),
            .timeout_ticks = 0,
        };
        ESP_GOTO_ON_ERROR(parlio_new_rx_soft_delimiter(&rx_delimiter_config, &esp_cam_io_parl->rx_delimiter), err, TAG, "Failed to initialize rx software delimiter");
        esp_cam_io_parl->use_soft_delimiter = true;
    }

    ESP_GOTO_ON_ERROR(register_partial_callback(esp_cam_io_parl), err, TAG, "Failed to initialize partial receive callback");

    for (int i = 0; i < ESP_CAM_IO_PARL_QUEUE_MAX; i++) {
        int queue_length = (i == ESP_CAM_IO_PARL_QUEUE_FAIL ? MAX(config->queue_frames, 2) : config->queue_frames); // Force minimum queue length to 2 for error queue to prevent heap leaks
        esp_cam_io_parl->queue_handle[i] = xQueueCreateWithCaps(queue_length, sizeof(esp_cam_io_parl_trans_t), MALLOC_CAP_DEFAULT);
        ESP_GOTO_ON_FALSE(esp_cam_io_parl->queue_handle[i], ESP_ERR_NO_MEM, err, TAG, "No memory for queue");
    }

    esp_cam_io_parl->chunk_free_queue = xQueueCreateWithCaps(
        ESP_CAM_IO_PARL_CHUNK_COUNT, sizeof(uint8_t),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    esp_cam_io_parl->chunk_ready_queue = xQueueCreateWithCaps(
        ESP_CAM_IO_PARL_CHUNK_COUNT, sizeof(esp_cam_io_parl_chunk_t),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_GOTO_ON_FALSE(esp_cam_io_parl->chunk_free_queue &&
                      esp_cam_io_parl->chunk_ready_queue,
                      ESP_ERR_NO_MEM, err, TAG, "No memory for ISR chunk queues");
    /* Static staging ring: cannot fail even with a fragmented internal heap. */
    esp_cam_io_parl->chunk_storage = s_chunk_storage;
    for (uint8_t slot = 0; slot < ESP_CAM_IO_PARL_CHUNK_COUNT; ++slot) {
        ESP_GOTO_ON_FALSE(xQueueSend(esp_cam_io_parl->chunk_free_queue, &slot, 0) == pdTRUE,
                          ESP_ERR_NO_MEM, err, TAG, "Failed to seed ISR chunk queue");
    }

    /* Static DMA payload: cannot fail and is reused across camera sessions. */
    esp_cam_io_parl->payload = s_payload;

    esp_cam_io_parl->alloc_heap_caps = config->frame_heap_caps ? config->frame_heap_caps : MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;

    *ret_handle = esp_cam_io_parl;
    return ESP_OK;
err:
    if (esp_cam_io_parl) {
        esp_cam_destroy_io_parl(esp_cam_io_parl);
    }
    return ret;
}

esp_err_t esp_cam_del_io_parl(esp_cam_io_parl_handle_t esp_cam_io_parl) {
    ESP_RETURN_ON_FALSE(esp_cam_io_parl, ESP_ERR_INVALID_ARG, TAG, "Invalid arguments");
    ESP_RETURN_ON_FALSE(!esp_cam_io_parl->cam_task_handle, ESP_ERR_INVALID_STATE,
                        TAG, "Camera task is still running");
    return esp_cam_destroy_io_parl(esp_cam_io_parl);
}

esp_err_t esp_cam_io_parl_set_alloc_size(esp_cam_io_parl_handle_t esp_cam_io_parl, uint32_t alloc_size, uint32_t heap_caps) {
    ESP_RETURN_ON_FALSE(esp_cam_io_parl && alloc_size > MIN_FRAME_ALLOC_SIZE, ESP_ERR_INVALID_ARG, TAG, "Invalid arguments");
    esp_cam_io_parl->alloc_size = alloc_size;
    if (heap_caps) {
        esp_cam_io_parl->alloc_heap_caps = heap_caps;
    }
    return ESP_OK;
}

esp_err_t esp_cam_io_parl_enable(esp_cam_io_parl_handle_t esp_cam_io_parl, bool reset_queue) {
    ESP_RETURN_ON_FALSE(esp_cam_io_parl, ESP_ERR_INVALID_ARG, TAG, "Invalid arguments");
    ESP_RETURN_ON_FALSE(__atomic_load_n(&esp_cam_io_parl->callback_inflight,
                                        __ATOMIC_ACQUIRE) == 0U,
                        ESP_ERR_INVALID_STATE, TAG,
                        "PARLIO callback is still active");
    __atomic_store_n(&esp_cam_io_parl->cam_task_stop, false, __ATOMIC_RELEASE);
    __atomic_store_n(&esp_cam_io_parl->callback_accepting, false, __ATOMIC_RELEASE);
    __atomic_store_n(&esp_cam_io_parl->rx_started, false, __ATOMIC_RELEASE);
    while (esp_cam_io_parl->cam_task_exit != NULL &&
           xSemaphoreTake(esp_cam_io_parl->cam_task_exit, 0) == pdTRUE) {
    }
    /* The staging ring is a static .bss buffer; just make sure the handle
     * points at it (it stays valid across disable/enable cycles). */
    if (esp_cam_io_parl->chunk_storage == NULL) {
        esp_cam_io_parl->chunk_storage = s_chunk_storage;
    }
    ESP_RETURN_ON_ERROR(register_partial_callback(esp_cam_io_parl), TAG, "Failed to restore partial receive callback");
    ESP_RETURN_ON_ERROR(parlio_rx_unit_enable(esp_cam_io_parl->rx_unit, reset_queue), TAG, "Failed to enable PARLIO RX unit");
    /* Keep DMA stopped until the sensor has finished PLL/crop setup. The
     * parser task is created now so sensor_io_parl_connect() can validate the
     * DVP handle without receiving an empty startup transaction.
     * Its stack lives in PSRAM on purpose: with XiaoZhi running, a 16 KB
     * internal stack (4096 words on this kernel) can starve the WebSocket /
     * audio allocations, which is exactly the failure seen when starting
     * XiaoZhi after using the camera. */
    ESP_RETURN_ON_FALSE(xTaskCreatePinnedToCoreWithCaps(
                            esp_cam_io_parl_task, "esp_cam_io_parl_task", 4096,
                            esp_cam_io_parl, 2, &esp_cam_io_parl->cam_task_handle,
                            tskNO_AFFINITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
                        ESP_ERR_NO_MEM, TAG, "Failed to allocate camera task");
    return ESP_OK;
}

esp_err_t esp_cam_io_parl_start(esp_cam_io_parl_handle_t esp_cam_io_parl)
{
    ESP_RETURN_ON_FALSE(esp_cam_io_parl && esp_cam_io_parl->cam_task_handle,
                        ESP_ERR_INVALID_STATE, TAG,
                        "PARLIO task is not enabled");
    ESP_RETURN_ON_FALSE(!__atomic_load_n(&esp_cam_io_parl->rx_started,
                                         __ATOMIC_ACQUIRE),
                        ESP_ERR_INVALID_STATE, TAG,
                        "PARLIO RX is already started");

    __atomic_store_n(&esp_cam_io_parl->callback_accepting, true, __ATOMIC_RELEASE);
    if (esp_cam_io_parl->use_soft_delimiter) {
        esp_err_t ret = parlio_rx_soft_delimiter_start_stop(
            esp_cam_io_parl->rx_unit, esp_cam_io_parl->rx_delimiter, true);
        if (ret != ESP_OK) {
            __atomic_store_n(&esp_cam_io_parl->callback_accepting, false,
                             __ATOMIC_RELEASE);
            return ret;
        }
    }

    const parlio_receive_config_t receive_config = {
        .delimiter = esp_cam_io_parl->rx_delimiter,
        .flags = {
            .partial_rx_en = true,
            .indirect_mount = false,
        },
    };
    esp_err_t ret = parlio_rx_unit_receive(esp_cam_io_parl->rx_unit,
                                           esp_cam_io_parl->payload,
                                           esp_cam_io_parl->payload_size,
                                           &receive_config);
    if (ret != ESP_OK) {
        if (esp_cam_io_parl->use_soft_delimiter) {
            (void)parlio_rx_soft_delimiter_start_stop(
                esp_cam_io_parl->rx_unit, esp_cam_io_parl->rx_delimiter, false);
        }
        __atomic_store_n(&esp_cam_io_parl->callback_accepting, false,
                         __ATOMIC_RELEASE);
        return ret;
    }
    __atomic_store_n(&esp_cam_io_parl->rx_started, true, __ATOMIC_RELEASE);
    return ESP_OK;
}

esp_err_t esp_cam_io_parl_disable(esp_cam_io_parl_handle_t esp_cam_io_parl) {
    ESP_RETURN_ON_FALSE(esp_cam_io_parl, ESP_ERR_INVALID_ARG, TAG, "Invalid arguments");
    esp_err_t ret = ESP_OK;
    __atomic_store_n(&esp_cam_io_parl->cam_task_stop, true, __ATOMIC_RELEASE);
    portENTER_CRITICAL(&s_callback_lock);
    __atomic_store_n(&esp_cam_io_parl->callback_accepting, false, __ATOMIC_RELEASE);
    portEXIT_CRITICAL(&s_callback_lock);
    if (esp_cam_io_parl->use_soft_delimiter &&
        __atomic_load_n(&esp_cam_io_parl->rx_started, __ATOMIC_ACQUIRE)) {
        esp_err_t stop_ret = parlio_rx_soft_delimiter_start_stop(
            esp_cam_io_parl->rx_unit, esp_cam_io_parl->rx_delimiter, false);
        if (stop_ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to stop PARLIO RX soft delimiter: %s",
                     esp_err_to_name(stop_ret));
            ret = stop_ret;
        }
    }
    __atomic_store_n(&esp_cam_io_parl->rx_started, false, __ATOMIC_RELEASE);
    esp_err_t disable_ret = parlio_rx_unit_disable(esp_cam_io_parl->rx_unit);
    if (disable_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to disable PARLIO RX unit: %s",
                 esp_err_to_name(disable_ret));
        ret = disable_ret;
    }
    /* gdma_stop() prevents new RX interrupts, but a callback already running
     * on the other core may still be touching the staging queues. Wait for it
     * before unregistering callbacks or deleting any queue/storage. */
    wait_callbacks_idle(esp_cam_io_parl);
    parlio_rx_event_callbacks_t empty_cbs = {0};
    esp_err_t callback_ret = parlio_rx_unit_register_event_callbacks(
        esp_cam_io_parl->rx_unit, &empty_cbs, NULL);
    if (callback_ret != ESP_OK && callback_ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Failed to clear PARLIO callbacks: %s",
                 esp_err_to_name(callback_ret));
        ret = ret == ESP_OK ? callback_ret : ret;
    }
    if (esp_cam_io_parl->cam_task_handle) {
        if (esp_cam_io_parl->cam_task_exit == NULL ||
            xSemaphoreTake(esp_cam_io_parl->cam_task_exit, portMAX_DELAY) != pdTRUE) {
            ESP_LOGE(TAG, "Camera task did not exit before PARLIO teardown");
            return ret == ESP_OK ? ESP_ERR_TIMEOUT : ret;
        }
        TaskHandle_t task = esp_cam_io_parl->cam_task_handle;
        vTaskSuspend(task);
        vTaskDeleteWithCaps(task);
        esp_cam_io_parl->cam_task_handle = NULL;
    }
    /* The parser task is gone, so no one can touch the frame state anymore.
     * Release it now instead of leaving it for destroy: the handle may be
     * disabled and re-enabled for the next camera session (the PARLIO RX unit
     * is created once and never destroyed per session), and both a partially
     * received JPEG and finished-but-unconsumed frames would otherwise leak on
     * every camera stop. */
    esp_cam_release_frame_buffers(esp_cam_io_parl);
    reset_capture(esp_cam_io_parl);
    drain_chunk_queues(esp_cam_io_parl);
    /* Keep the static staging ring; it is reused by the next enable(). */
    return ret;
}

esp_err_t esp_cam_io_parl_receive(esp_cam_io_parl_handle_t esp_cam_io_parl, esp_cam_io_parl_trans_t *frame, int32_t timeout_ms) {
    ESP_RETURN_ON_FALSE(esp_cam_io_parl && frame, ESP_ERR_INVALID_ARG, TAG, "Invalid arguments");
    //ESP_RETURN_ON_FALSE(esp_cam_io_parl->config.sampling_mode == ESP_CAM_IO_PARL_BUFFER, ESP_ERR_INVALID_STATE, TAG, "Sampling mode is invalid");
    TickType_t ticks = timeout_ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    BaseType_t ret = xQueueReceive(esp_cam_io_parl->queue_handle[ESP_CAM_IO_PARL_QUEUE_READY], frame, ticks);
    if (ret == pdFALSE) {
        frame->buffer = NULL;
        frame->length = 0;
    }
    return ret == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t esp_cam_io_parl_receive_from_isr(esp_cam_io_parl_handle_t esp_cam_io_parl, esp_cam_io_parl_trans_t *frame, bool *hp_task_woken) {
    ESP_CAM_IO_PARL_CHECK_ISR(esp_cam_io_parl && frame, ESP_ERR_INVALID_ARG);
    ESP_CAM_IO_PARL_CHECK_ISR(xPortInIsrContext() == pdTRUE, ESP_ERR_INVALID_STATE);
    //ESP_CAM_IO_PARL_CHECK_ISR(esp_cam_io_parl->config.sampling_mode == ESP_CAM_IO_PARL_BUFFER, ESP_ERR_INVALID_STATE);
    BaseType_t _hp_task_woken = 0;
    BaseType_t ret = xQueueReceiveFromISR(esp_cam_io_parl->queue_handle[ESP_CAM_IO_PARL_QUEUE_READY], frame, &_hp_task_woken);
    if (hp_task_woken) {
        *hp_task_woken = _hp_task_woken != 0;
    }
    return ret == pdTRUE ? ESP_OK : ESP_FAIL;
}

esp_err_t esp_cam_io_parl_free_buffer(esp_cam_io_parl_trans_t *frame) {
    ESP_RETURN_ON_FALSE(frame->buffer, ESP_ERR_INVALID_ARG, TAG, "Invalid arguments");
    heap_caps_free(frame->buffer);
    frame->buffer = NULL;
    frame->length = 0;
    return ESP_OK;
}
