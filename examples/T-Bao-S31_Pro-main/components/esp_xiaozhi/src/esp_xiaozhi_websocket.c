/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <esp_check.h>
#include <esp_log.h>
#include <esp_websocket_client.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <freertos/idf_additions.h>

#include <stdlib.h>
#include <inttypes.h>
#include <string.h>

#include "esp_xiaozhi_keystore.h"
#include "esp_xiaozhi_board.h"
#include "esp_xiaozhi_websocket.h"
#include "esp_websocket_client_psram.h"

/* The socket callback never owns a heap buffer. Complete messages are copied
 * into one of these fixed PSRAM-backed slots and released by the dispatch task
 * after the application callback returns. */
/* Keep the fixed reservation below the size of the largest normal MCP frame.
 * Two 16 KiB slots are enough for hello/tools/MCP JSON and audio control
 * messages, while using half the PSRAM of the old 32 KiB-per-slot pool.  The
 * pool is retained until websocket deinit so reconnects never fragment PSRAM
 * by repeatedly allocating and freeing these blocks. */
#define ESP_XIAOZHI_WEBSOCKET_INCOMING_POOL_BLOCK_SIZE (16U * 1024U)
/* Two slots were not enough: TTS audio frames arrive at a high rate and one
 * slow JSON dispatch (MCP/emote) let both fill, so the next TEXT frame —
 * e.g. tts start — was silently dropped. The session then ran with a stale
 * tts_playing flag (no mic mute, no barge-in). Four 16 KiB PSRAM slots ride
 * out those bursts; the pool is allocated once and reused across sessions. */
#define ESP_XIAOZHI_WEBSOCKET_INCOMING_POOL_BLOCK_COUNT 4U
#define ESP_XIAOZHI_WEBSOCKET_INCOMING_INVALID_SLOT 0xFFU
#define ESP_XIAOZHI_WEBSOCKET_INCOMING_INVALID_OPCODE 0xFFU
#define ESP_XIAOZHI_WEBSOCKET_INCOMING_EVENT_CONNECTED 0xF0U
#define ESP_XIAOZHI_WEBSOCKET_INCOMING_EVENT_DISCONNECTED 0xF1U
#define ESP_XIAOZHI_WEBSOCKET_INCOMING_EVENT_ERROR 0xF2U
#define ESP_XIAOZHI_WEBSOCKET_INCOMING_EVENT_FINISH 0xF3U
#define ESP_XIAOZHI_WEBSOCKET_INCOMING_EVENT_STOP 0xFEU

typedef struct {
    uint8_t opcode;
    size_t len;
    uint8_t slot;
} esp_xiaozhi_websocket_incoming_message_t;

typedef struct {
    uint8_t type;
    esp_err_t error;
} esp_xiaozhi_websocket_incoming_event_t;

typedef struct esp_xiaozhi_websocket_s {
    char url[ESP_XIAOZHI_WEBSOCKET_MAX_STRING_SIZE];
    char token[ESP_XIAOZHI_WEBSOCKET_MAX_STRING_SIZE];
    char protocol_version[16];
    char headers[ESP_XIAOZHI_WEBSOCKET_MAX_HEADER_SIZE];
    volatile bool websocket_connected;
    volatile bool connection_started;
    volatile bool finished;
    volatile bool stopping;
    volatile bool disconnect_notified;

    esp_websocket_client_handle_t websocket_client;
    esp_xiaozhi_transport_handle_t transport_handle; /*!< Passed to callbacks so chat can resolve by transport handle */
    esp_xiaozhi_transport_callbacks_t callbacks;
    EventGroupHandle_t lifecycle_events;
    SemaphoreHandle_t lifecycle_mutex;

    /* The IDF websocket receive task holds client->lock while it dispatches
     * events. The socket callback only copies into a fixed PSRAM pool or
     * queues a small event; JSON/MCP/UI callbacks run on the worker below. */
    QueueHandle_t incoming_queue;
    QueueHandle_t incoming_event_queue;
    QueueHandle_t incoming_free_queue;
    SemaphoreHandle_t incoming_state_mutex;
    SemaphoreHandle_t incoming_exited;
    TaskHandle_t incoming_task;
    bool incoming_task_with_caps;
    bool incoming_task_static;
    StackType_t *incoming_task_stack;
    /* A static internal stack is reserved once and reused for every
     * conversation. Keeping this separate from incoming_task_stack lets
     * stop/restart clear the live task metadata without returning the block to
     * a fragmented heap. */
    StackType_t *incoming_task_stack_storage;
    bool incoming_task_stack_storage_psram;
    /* Keep the TCB out of the dynamic internal heap. The websocket wrapper
     * is created before a session starts, so this storage is stable for the
     * whole lifetime of the transport object and can be reused on retries. */
#if configSUPPORT_STATIC_ALLOCATION == 1
    StaticTask_t incoming_task_tcb;
#endif
    volatile bool incoming_accepting;
    volatile uint32_t incoming_event_inflight;
    volatile bool incoming_stop_requested;
    /* A FreeRTOS task can run before xTaskCreate*() returns.  Keep the entry
     * function behind this publication gate until the owner has filled every
     * handle/stack/reclaimer field below. */
    volatile bool incoming_task_published;

    uint8_t *incoming_pool[ESP_XIAOZHI_WEBSOCKET_INCOMING_POOL_BLOCK_COUNT];
    uint8_t incoming_assembly_slot;
    uint8_t incoming_assembly_opcode;
    size_t incoming_assembly_len;
    uint8_t incoming_frame_opcode;
    size_t incoming_frame_base;
    size_t incoming_frame_len;
    uint32_t incoming_pool_in_use_mask;
    uint8_t incoming_pool_in_use;
    uint8_t incoming_pool_high_watermark;
    uint32_t incoming_pool_drops;
    volatile bool incoming_pool_ready;
} esp_xiaozhi_websocket_t;

static const char *TAG = "ESP_XIAOZHI_WEBSOCKET";

/* The socket callback and the stop/restart owner must agree on the exact
 * moment at which lifecycle events stop being accepted.  A FreeRTOS mutex is
 * intentionally not used for this tiny gate: the callback must never wait
 * behind the receive worker while it is copying a frame. */
static portMUX_TYPE s_incoming_event_lock = portMUX_INITIALIZER_UNLOCKED;

#define ESP_XIAOZHI_WEBSOCKET_CONNECTED_BIT BIT0
#define ESP_XIAOZHI_WEBSOCKET_FINISHED_BIT BIT1
#define ESP_XIAOZHI_WEBSOCKET_CONNECT_TIMEOUT_MS 10000
#define ESP_XIAOZHI_WEBSOCKET_TEXT_SEND_TIMEOUT_MS 2000U
#define ESP_XIAOZHI_WEBSOCKET_BINARY_SEND_TIMEOUT_MS 150U
#define ESP_XIAOZHI_WEBSOCKET_INCOMING_QUEUE_DEPTH 24U
#define ESP_XIAOZHI_WEBSOCKET_INCOMING_EVENT_QUEUE_DEPTH 8U
/* Each complete WebSocket message occupies one preallocated PSRAM slot. Two
 * 16 KiB slots bound the receive reservation to 32 KiB so the UI/emote assets
 * retain a contiguous PSRAM region. Messages larger than a slot are rejected
 * by the reassembly guard and counted; normal MCP and audio frames are below
 * this limit. The pool is allocated lazily on the first real WebSocket start,
 * after boot-time UI assets have had a chance to claim memory, then retained
 * for all later reconnects. */
/* The receive worker runs MCP JSON parsing and may synchronously send the MCP
 * response, which descends through MbedTLS/PSA AES-GCM and can disable the
 * flash/PSRAM cache. Its stack must therefore stay in internal DRAM. The
 * receive payload pool below remains PSRAM-backed, so the stack reservation is
 * the only internal cost of this task. */
#define ESP_XIAOZHI_WEBSOCKET_INCOMING_TASK_STACK (8 * 1024)
/* Above the emote renderer (5): when the renderer wedges on a lost flush
 * completion it busy-spins on CPU0 and must not starve the message pump,
 * otherwise every incoming server message (tts start/stop, MCP) is ignored
 * and the session goes silent mid-conversation. */
#define ESP_XIAOZHI_WEBSOCKET_INCOMING_TASK_PRIORITY 6
#define ESP_XIAOZHI_WEBSOCKET_INCOMING_TASK_CORE 0
#define ESP_XIAOZHI_WEBSOCKET_INCOMING_TASK_CREATE_MAX_ATTEMPTS 3U
#define ESP_XIAOZHI_WEBSOCKET_INCOMING_TASK_CREATE_RETRY_DELAY_MS 100U
#define ESP_XIAOZHI_WEBSOCKET_INCOMING_TASK_EXIT_TIMEOUT_MS 5000U

/* Internal-RAM reserve owned by the component rather than by a websocket
 * instance.  It is allocated on first use and retained for the lifetime of the
 * firmware, so re-creating the receive task can never fail with ESP_ERR_NO_MEM
 * even after camera / face-follow use has fragmented the internal heap
 * (observed: free=5835 B, largest=1008 B).  The chat is a singleton and the
 * previous websocket is deinitialized before a new one starts, so only one
 * receive worker can use this buffer at a time.
 *
 * Deliberately not a .bss array: a permanent 8 KiB reservation is taken from
 * the internal heap even when XiaoZhi has never opened a session, and on this
 * board the UI/emote/AFE already leave only ~15-25 KiB free.  That missing
 * 8 KiB was the difference between the face-follow startup burst fitting and
 * the WiFi power-save path aborting in esp_phy/esp_timer_create.  Allocating
 * it at the first session keeps the wake-after-camera guarantee without
 * taxing boots that never talk to the server. */
static StackType_t *s_incoming_task_stack_reserve = NULL;
static size_t s_incoming_task_stack_reserve_size = 0;

static size_t esp_xiaozhi_websocket_task_stack_bytes(
    configSTACK_DEPTH_TYPE stack_depth)
{
    return (size_t)stack_depth * sizeof(StackType_t);
}

static void esp_xiaozhi_websocket_incoming_task(void *arg);
static esp_err_t esp_xiaozhi_websocket_stop_incoming_task(
    esp_xiaozhi_websocket_t *websocket);

static void esp_xiaozhi_websocket_publish_incoming_task(
    esp_xiaozhi_websocket_t *websocket)
{
    if (websocket != NULL) {
        __atomic_store_n(&websocket->incoming_task_published, true,
                         __ATOMIC_RELEASE);
    }
}

static bool esp_xiaozhi_websocket_incoming_accepting(
    const esp_xiaozhi_websocket_t *websocket)
{
    return websocket != NULL &&
           __atomic_load_n(&websocket->incoming_accepting, __ATOMIC_ACQUIRE);
}

static void esp_xiaozhi_websocket_set_incoming_accepting(
    esp_xiaozhi_websocket_t *websocket, bool accepting)
{
    if (websocket == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_incoming_event_lock);
    __atomic_store_n(&websocket->incoming_accepting, accepting,
                     __ATOMIC_RELEASE);
    portEXIT_CRITICAL(&s_incoming_event_lock);
}

/* Lifecycle events do not take the worker state mutex in the socket callback,
 * so stop() uses this short admission counter to close the final enqueue
 * window before flushing the event queue. The callback never blocks waiting for
 * the worker; it only performs an atomic counter transition and a zero-timeout
 * queue send. */
static bool esp_xiaozhi_websocket_event_callback_enter(
    esp_xiaozhi_websocket_t *websocket)
{
    if (websocket == NULL) {
        return false;
    }

    bool accepted = false;
    portENTER_CRITICAL(&s_incoming_event_lock);
    if (__atomic_load_n(&websocket->incoming_accepting, __ATOMIC_ACQUIRE)) {
        __atomic_add_fetch(&websocket->incoming_event_inflight, 1U,
                           __ATOMIC_ACQ_REL);
        accepted = true;
    }
    portEXIT_CRITICAL(&s_incoming_event_lock);
    return accepted;
}

static void esp_xiaozhi_websocket_event_callback_leave(
    esp_xiaozhi_websocket_t *websocket)
{
    if (websocket != NULL) {
        __atomic_sub_fetch(&websocket->incoming_event_inflight, 1U,
                           __ATOMIC_RELEASE);
    }
}

static bool esp_xiaozhi_websocket_wait_event_callbacks_idle(
    esp_xiaozhi_websocket_t *websocket)
{
    if (websocket == NULL) {
        return true;
    }

    const TickType_t retry_delay = pdMS_TO_TICKS(1);
    for (unsigned attempt = 0; attempt < 1000U; ++attempt) {
        if (__atomic_load_n(&websocket->incoming_event_inflight,
                            __ATOMIC_ACQUIRE) == 0U) {
            return true;
        }
        if (retry_delay != 0) {
            vTaskDelay(retry_delay);
        } else {
            taskYIELD();
        }
    }
    return __atomic_load_n(&websocket->incoming_event_inflight,
                           __ATOMIC_ACQUIRE) == 0U;
}

static bool esp_xiaozhi_websocket_incoming_task_is_published(
    const esp_xiaozhi_websocket_t *websocket)
{
    return websocket != NULL &&
           __atomic_load_n(&websocket->incoming_task_published,
                           __ATOMIC_ACQUIRE);
}

static bool esp_xiaozhi_websocket_task_stack_caps_are_internal(
    UBaseType_t memory_caps)
{
    if ((memory_caps & MALLOC_CAP_SPIRAM) != 0U ||
            (memory_caps & MALLOC_CAP_INTERNAL) == 0U) {
        ESP_LOGE(TAG,
                 "Rejecting external/non-internal WebSocket receive task stack "
                 "caps=0x%08x",
                 (unsigned)memory_caps);
        return false;
    }
    return true;
}

static BaseType_t esp_xiaozhi_websocket_create_task_with_caps(
    TaskFunction_t task_entry,
    const char *task_name,
    configSTACK_DEPTH_TYPE stack_depth,
    void *task_arg,
    UBaseType_t priority,
    TaskHandle_t *task_handle,
    BaseType_t core_id,
    UBaseType_t memory_caps)
{
    /* A task without a published handle cannot be stopped/reclaimed safely.
     * All callers in this component provide storage; reject future misuse
     * instead of creating an orphaned external stack. */
    if (task_handle == NULL) {
        return pdFAIL;
    }
    if (!esp_xiaozhi_websocket_task_stack_caps_are_internal(memory_caps)) {
        return pdFAIL;
    }
#if configSUPPORT_STATIC_ALLOCATION == 1
    TaskHandle_t created_task = NULL;
    const BaseType_t ret = xTaskCreatePinnedToCoreWithCaps(
        task_entry, task_name, stack_depth, task_arg, priority, &created_task,
        core_id, memory_caps);
    if (created_task != NULL) {
        *task_handle = created_task;
    } else {
        *task_handle = NULL;
    }
    return ret;
#else
    (void)memory_caps;
    TaskHandle_t created_task = NULL;
    const BaseType_t ret = xTaskCreatePinnedToCore(
        task_entry, task_name, stack_depth, task_arg, priority, &created_task,
        core_id);
    if (created_task != NULL) {
        *task_handle = created_task;
    } else {
        *task_handle = NULL;
    }
    return ret;
#endif
}

static BaseType_t esp_xiaozhi_websocket_create_static_task_with_caps(
    esp_xiaozhi_websocket_t *websocket,
    TaskFunction_t task_entry,
    const char *task_name,
    configSTACK_DEPTH_TYPE stack_depth,
    void *task_arg,
    UBaseType_t priority,
    TaskHandle_t *task_handle,
    BaseType_t core_id,
    UBaseType_t memory_caps)
{
#if configSUPPORT_STATIC_ALLOCATION == 1
    if (websocket == NULL || task_handle == NULL) {
        return pdFAIL;
    }
    if (!esp_xiaozhi_websocket_task_stack_caps_are_internal(memory_caps)) {
        return pdFAIL;
    }

    /* FreeRTOS expresses stack depth in StackType_t entries. ESP32-S31 uses a
     * one-byte StackType_t, but keep the allocator correct on wider ports. */
    const size_t stack_bytes =
        esp_xiaozhi_websocket_task_stack_bytes(stack_depth);
    const bool wants_psram = (memory_caps & MALLOC_CAP_SPIRAM) != 0;
    StackType_t *stack = NULL;

    if (!wants_psram) {
        /* One-shot internal reserve, then reused for every later session.  The
         * allocation only has to succeed once; after that the stack never
         * depends on heap state again. */
        StackType_t *reserve =
            __atomic_load_n(&s_incoming_task_stack_reserve, __ATOMIC_ACQUIRE);
        if (reserve != NULL && s_incoming_task_stack_reserve_size < stack_bytes) {
            ESP_LOGE(TAG,
                     "Internal receive task stack reserve is too small "
                     "(%u < %u bytes)",
                     (unsigned)s_incoming_task_stack_reserve_size,
                     (unsigned)stack_bytes);
            return pdFAIL;
        }
        if (reserve == NULL) {
            StackType_t *allocated =
                (StackType_t *)heap_caps_malloc(stack_bytes, memory_caps);
            if (allocated == NULL) {
                ESP_LOGW(TAG,
                         "Internal receive task stack reserve allocation "
                         "failed (%u bytes; internal free=%u largest=%u)",
                         (unsigned)stack_bytes,
                         (unsigned)heap_caps_get_free_size(
                             MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                         (unsigned)heap_caps_get_largest_free_block(
                             MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
                return pdFAIL;
            }
            StackType_t *expected = NULL;
            /* Publish the size before the pointer: any reader that observes a
             * non-NULL reserve must also observe a valid size. */
            s_incoming_task_stack_reserve_size = stack_bytes;
            if (__atomic_compare_exchange_n(
                    &s_incoming_task_stack_reserve, &expected, allocated,
                    false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                reserve = allocated;
                ESP_LOGI(TAG,
                         "Reserved reusable WebSocket receive task stack in "
                         "internal RAM (%u bytes, stack=%p)",
                         (unsigned)stack_bytes, (void *)allocated);
            } else {
                /* Lost a race with another start; the winner's reserve is
                 * used and the duplicate is returned. */
                heap_caps_free(allocated);
                reserve = expected;
            }
        }
        stack = reserve;
    } else {
        stack = websocket->incoming_task_stack_storage;

        /* The reserve survives a failed create, but it is still tied to the
         * capability class it was allocated from.  Keep this check even though
         * WebSocket task stacks are currently internal-only: it prevents a
         * stale external reserve from ever being reused for a cache-sensitive
         * worker. */
        if (stack != NULL &&
                websocket->incoming_task_stack_storage_psram != wants_psram) {
            if (websocket->incoming_task != NULL ||
                    websocket->incoming_task_stack != NULL) {
                ESP_LOGE(TAG,
                         "Cannot replace WebSocket receive task stack reserve "
                         "while task is active (task=%p stack=%p)",
                         (void *)websocket->incoming_task,
                         (void *)websocket->incoming_task_stack);
                return pdFAIL;
            }
            ESP_LOGW(TAG,
                     "Replacing WebSocket receive task stack reserve (%s -> %s)",
                     websocket->incoming_task_stack_storage_psram
                         ? "PSRAM" : "internal RAM",
                     wants_psram ? "PSRAM" : "internal RAM");
            heap_caps_free(stack);
            websocket->incoming_task_stack_storage = NULL;
            websocket->incoming_task_stack_storage_psram = false;
            stack = NULL;
        }

        if (stack == NULL) {
            stack = (StackType_t *)heap_caps_malloc(stack_bytes, memory_caps);
            if (stack == NULL) {
                return pdFAIL;
            }
            websocket->incoming_task_stack_storage = stack;
            websocket->incoming_task_stack_storage_psram = wants_psram;
            ESP_LOGI(TAG,
                     "Reserved reusable WebSocket receive task stack in %s (%u bytes)",
                     websocket->incoming_task_stack_storage_psram ? "PSRAM" : "internal RAM",
                     (unsigned)stack_bytes);
        } else {
            ESP_LOGD(TAG,
                     "Reusing WebSocket receive task stack in %s (%u bytes)",
                     websocket->incoming_task_stack_storage_psram ? "PSRAM" : "internal RAM",
                     (unsigned)stack_bytes);
        }
    }

    TaskHandle_t task = xTaskCreateStaticPinnedToCore(
        task_entry, task_name, (uint32_t)stack_depth, task_arg, priority,
        stack, &websocket->incoming_task_tcb, core_id);
    if (task == NULL) {
        /* Keep the reserved stack for the next bounded retry.  Returning it
         * here would recreate the PSRAM fragmentation that this path avoids. */
        return pdFAIL;
    }

    websocket->incoming_task_stack = stack;
    websocket->incoming_task_static = true;
    *task_handle = task;
    return pdPASS;
#else
    (void)websocket;
    (void)task_entry;
    (void)task_name;
    (void)stack_depth;
    (void)task_arg;
    (void)priority;
    (void)task_handle;
    (void)core_id;
    (void)memory_caps;
    return pdFAIL;
#endif
}

static void esp_xiaozhi_websocket_wait_task_not_running(TaskHandle_t task)
{
    if (task == NULL) {
        return;
    }

    for (;;) {
        bool running = false;
        for (BaseType_t core = 0; core < configNUMBER_OF_CORES; ++core) {
            if (xTaskGetCurrentTaskHandleForCore(core) == task) {
                running = true;
                break;
            }
        }
        if (!running) {
            break;
        }
        /* Give the idle task a tick while the other core releases the task's
         * stack; a tight yield loop can starve watchdog users. */
        vTaskDelay(1);
    }
}

static void esp_xiaozhi_websocket_log_memory(const char *reason)
{
    ESP_LOGI(TAG,
             "WebSocket memory (%s): internal free=%u largest=%u; "
             "PSRAM free=%u largest=%u",
             reason != NULL ? reason : "snapshot",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

/* Pool counters are sampled by the owner after a worker transition.  Keeping
 * this out of the socket callback preserves the callback's bounded copy/queue
 * handoff while still exposing drops and high-water usage in the next log. */
static void esp_xiaozhi_websocket_log_incoming_stats(
    const esp_xiaozhi_websocket_t *websocket, const char *reason)
{
    if (websocket == NULL) {
        return;
    }
    ESP_LOGI(TAG,
             "WebSocket receive pool stats (%s): ready=%d in_use=%u high=%u drops=%u",
             reason != NULL ? reason : "snapshot",
             websocket->incoming_pool_ready ? 1 : 0,
             (unsigned)websocket->incoming_pool_in_use,
             (unsigned)websocket->incoming_pool_high_watermark,
             (unsigned)websocket->incoming_pool_drops);
    esp_xiaozhi_websocket_log_memory(reason);
}

static void esp_xiaozhi_websocket_rebuild_free_queue_locked(
    esp_xiaozhi_websocket_t *websocket)
{
    if (websocket == NULL || websocket->incoming_free_queue == NULL) {
        return;
    }

    /* The bit mask is the ownership authority. Rebuild the tiny index queue
     * after every ownership transition so a stale/duplicated queue item can
     * never consume a pool slot permanently. */
    uint8_t stale_slot;
    while (xQueueReceive(websocket->incoming_free_queue, &stale_slot, 0) == pdTRUE) {
    }

    for (uint8_t slot = 0;
            slot < ESP_XIAOZHI_WEBSOCKET_INCOMING_POOL_BLOCK_COUNT; ++slot) {
        const uint32_t slot_bit = 1UL << slot;
        if (websocket->incoming_pool[slot] == NULL ||
                (websocket->incoming_pool_in_use_mask & slot_bit) != 0U) {
            continue;
        }
        if (xQueueSend(websocket->incoming_free_queue, &slot, 0) != pdTRUE) {
            ++websocket->incoming_pool_drops;
            break;
        }
    }
}

static void esp_xiaozhi_websocket_recount_pool_locked(
    esp_xiaozhi_websocket_t *websocket)
{
    if (websocket == NULL) {
        return;
    }

    uint8_t in_use = 0;
    for (uint8_t slot = 0;
            slot < ESP_XIAOZHI_WEBSOCKET_INCOMING_POOL_BLOCK_COUNT; ++slot) {
        if ((websocket->incoming_pool_in_use_mask & (1UL << slot)) != 0U) {
            ++in_use;
        }
    }
    websocket->incoming_pool_in_use = in_use;
}

static void esp_xiaozhi_websocket_return_pool_slot_locked(
    esp_xiaozhi_websocket_t *websocket, uint8_t slot)
{
    if (websocket == NULL ||
            slot == ESP_XIAOZHI_WEBSOCKET_INCOMING_INVALID_SLOT) {
        return;
    }

    if (slot >= ESP_XIAOZHI_WEBSOCKET_INCOMING_POOL_BLOCK_COUNT) {
        ++websocket->incoming_pool_drops;
        return;
    }

    const uint32_t slot_bit = 1UL << slot;
    if ((websocket->incoming_pool_in_use_mask & slot_bit) == 0U) {
        ++websocket->incoming_pool_drops;
        return;
    }

    /* Clear ownership before touching the queue. A full/corrupt queue must
     * not turn a returned buffer into a permanently leaked slot. */
    websocket->incoming_pool_in_use_mask &= ~slot_bit;
    esp_xiaozhi_websocket_recount_pool_locked(websocket);
    esp_xiaozhi_websocket_rebuild_free_queue_locked(websocket);
}

static void esp_xiaozhi_websocket_free_incoming_message_locked(
    esp_xiaozhi_websocket_t *websocket,
    esp_xiaozhi_websocket_incoming_message_t *message)
{
    if (message == NULL) {
        return;
    }
    esp_xiaozhi_websocket_return_pool_slot_locked(websocket, message->slot);
    message->slot = ESP_XIAOZHI_WEBSOCKET_INCOMING_INVALID_SLOT;
    message->len = 0;
}

static void esp_xiaozhi_websocket_free_incoming_message(
    esp_xiaozhi_websocket_t *websocket,
    esp_xiaozhi_websocket_incoming_message_t *message)
{
    if (websocket == NULL || websocket->incoming_state_mutex == NULL) {
        return;
    }
    if (xSemaphoreTake(websocket->incoming_state_mutex, portMAX_DELAY) == pdTRUE) {
        esp_xiaozhi_websocket_free_incoming_message_locked(websocket, message);
        xSemaphoreGive(websocket->incoming_state_mutex);
    }
}

static void esp_xiaozhi_websocket_reset_assembly_locked(
    esp_xiaozhi_websocket_t *websocket)
{
    if (websocket->incoming_assembly_slot !=
            ESP_XIAOZHI_WEBSOCKET_INCOMING_INVALID_SLOT) {
        esp_xiaozhi_websocket_return_pool_slot_locked(
            websocket, websocket->incoming_assembly_slot);
    }
    websocket->incoming_assembly_slot =
        ESP_XIAOZHI_WEBSOCKET_INCOMING_INVALID_SLOT;
    websocket->incoming_assembly_opcode = 0;
    websocket->incoming_assembly_len = 0;
    websocket->incoming_frame_opcode =
        ESP_XIAOZHI_WEBSOCKET_INCOMING_INVALID_OPCODE;
    websocket->incoming_frame_base = 0;
    websocket->incoming_frame_len = 0;
}

static bool esp_xiaozhi_websocket_take_pool_slot_locked(
    esp_xiaozhi_websocket_t *websocket, uint8_t *slot)
{
    if (websocket == NULL || slot == NULL) {
        return false;
    }

    /* Scan the ownership mask instead of trusting queue contents. The queue is
     * rebuilt as a convenience index, while this mask remains the source of
     * truth even if an earlier queue operation was interrupted or duplicated. */
    uint8_t free_slot = ESP_XIAOZHI_WEBSOCKET_INCOMING_INVALID_SLOT;
    for (uint8_t candidate = 0;
            candidate < ESP_XIAOZHI_WEBSOCKET_INCOMING_POOL_BLOCK_COUNT;
            ++candidate) {
        const uint32_t candidate_bit = 1UL << candidate;
        if (websocket->incoming_pool[candidate] != NULL &&
                (websocket->incoming_pool_in_use_mask & candidate_bit) == 0U) {
            free_slot = candidate;
            break;
        }
    }
    if (free_slot == ESP_XIAOZHI_WEBSOCKET_INCOMING_INVALID_SLOT) {
        ++websocket->incoming_pool_drops;
        return false;
    }

    *slot = free_slot;
    const uint32_t slot_bit = 1UL << free_slot;
    websocket->incoming_pool_in_use_mask |= slot_bit;
    esp_xiaozhi_websocket_recount_pool_locked(websocket);
    if (websocket->incoming_pool_in_use > websocket->incoming_pool_high_watermark) {
        websocket->incoming_pool_high_watermark = websocket->incoming_pool_in_use;
    }
    esp_xiaozhi_websocket_rebuild_free_queue_locked(websocket);
    return true;
}

static bool esp_xiaozhi_websocket_queue_pool_slot_locked(
    esp_xiaozhi_websocket_t *websocket, uint8_t opcode,
    uint8_t slot, size_t data_len)
{
    esp_xiaozhi_websocket_incoming_message_t message = {
        .opcode = opcode,
        .len = data_len,
        .slot = slot,
    };
    if (xQueueSend(websocket->incoming_queue, &message, 0) == pdTRUE) {
        return true;
    }

    ++websocket->incoming_pool_drops;
    esp_xiaozhi_websocket_return_pool_slot_locked(websocket, slot);
    return false;
}

/* Socket-task event callbacks use the same fixed, non-owning handoff as data
 * frames. Application callbacks therefore never run under the IDF client lock.
 * FreeRTOS queues are internally synchronized, so do not take the pool mutex
 * here: a data copy can briefly hold it while the connected/disconnected event
 * is being emitted, and dropping that lifecycle event would strand the app in
 * the wrong wake state. The accepting flag is only a gate; stop() flushes the
 * queue after lowering it and the worker rechecks the flag before dispatch. */
static bool esp_xiaozhi_websocket_queue_event(
    esp_xiaozhi_websocket_t *websocket, uint8_t type, esp_err_t error)
{
    if (websocket == NULL || websocket->incoming_event_queue == NULL) {
        return false;
    }

    esp_xiaozhi_websocket_incoming_event_t event = {
        .type = type,
        .error = error,
    };
    if (!esp_xiaozhi_websocket_event_callback_enter(websocket)) {
        return false;
    }

    bool queued = xQueueSend(
        websocket->incoming_event_queue, &event, 0) == pdTRUE;
    if (!queued && (type == ESP_XIAOZHI_WEBSOCKET_INCOMING_EVENT_CONNECTED ||
                    type == ESP_XIAOZHI_WEBSOCKET_INCOMING_EVENT_DISCONNECTED ||
                    type == ESP_XIAOZHI_WEBSOCKET_INCOMING_EVENT_ERROR)) {
        /* Lifecycle state is edge-triggered and cannot be reconstructed from
         * a later data frame. If a busy parser filled the tiny event queue,
         * discard the oldest stale edge and retry the newest one once. The
         * callback remains bounded and allocation-free. */
        esp_xiaozhi_websocket_incoming_event_t stale_event;
        (void)xQueueReceive(websocket->incoming_event_queue, &stale_event, 0);
        queued = xQueueSend(websocket->incoming_event_queue, &event, 0) == pdTRUE;
    }
    esp_xiaozhi_websocket_event_callback_leave(websocket);
    if (!queued) {
        ++websocket->incoming_pool_drops;
    }
    return queued;
}

/* The queue owns slots, so flush every queued slot explicitly during a
 * session transition. No heap object is created or destroyed here. */
static void esp_xiaozhi_websocket_flush_incoming_locked(
    esp_xiaozhi_websocket_t *websocket)
{
    esp_xiaozhi_websocket_incoming_message_t message = {
        .slot = ESP_XIAOZHI_WEBSOCKET_INCOMING_INVALID_SLOT,
    };
    while (xQueueReceive(websocket->incoming_queue, &message, 0) == pdTRUE) {
        esp_xiaozhi_websocket_free_incoming_message_locked(websocket, &message);
    }
    esp_xiaozhi_websocket_reset_assembly_locked(websocket);
    if (websocket->incoming_event_queue != NULL) {
        esp_xiaozhi_websocket_incoming_event_t event;
        while (xQueueReceive(websocket->incoming_event_queue, &event, 0) == pdTRUE) {
        }
    }
    /* Reconcile the index queue with the ownership mask even when the queue
     * was already empty.  This keeps a slot returned by the worker visible to
     * the next session after a stop races a queued frame. */
    esp_xiaozhi_websocket_rebuild_free_queue_locked(websocket);
}

static bool esp_xiaozhi_websocket_init_incoming_pool(
    esp_xiaozhi_websocket_t *websocket)
{
    if (websocket == NULL || websocket->incoming_free_queue == NULL) {
        return false;
    }

    if (websocket->incoming_pool_ready) {
        return true;
    }

    /* A failed allocation may leave slot indices from a partial seed in the
     * free queue. Drain them before retrying so a later wake never receives a
     * pointer for a buffer that was already released. */
    uint8_t stale_slot;
    while (xQueueReceive(websocket->incoming_free_queue, &stale_slot, 0) == pdTRUE) {
    }
    websocket->incoming_pool_in_use_mask = 0;
    websocket->incoming_pool_in_use = 0;
    websocket->incoming_pool_high_watermark = 0;
    websocket->incoming_pool_ready = false;

    /* A previous start may have stopped after allocating only part of the
     * pool. Reconcile those pointers before retrying so a failed allocation
     * can never leak a block or overwrite an owned pointer. */
    for (uint8_t slot = 0;
            slot < ESP_XIAOZHI_WEBSOCKET_INCOMING_POOL_BLOCK_COUNT; ++slot) {
        if (websocket->incoming_pool[slot] != NULL) {
            heap_caps_free(websocket->incoming_pool[slot]);
            websocket->incoming_pool[slot] = NULL;
        }
    }

    uint8_t allocated_slots = 0;
    for (uint8_t slot = 0;
            slot < ESP_XIAOZHI_WEBSOCKET_INCOMING_POOL_BLOCK_COUNT; ++slot) {
        websocket->incoming_pool[slot] = heap_caps_malloc(
            ESP_XIAOZHI_WEBSOCKET_INCOMING_POOL_BLOCK_SIZE,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (websocket->incoming_pool[slot] == NULL) {
            ESP_LOGW(TAG,
                     "Failed to allocate WebSocket receive pool slot=%u (%u bytes)"
                     " from PSRAM",
                     (unsigned)slot,
                     (unsigned)ESP_XIAOZHI_WEBSOCKET_INCOMING_POOL_BLOCK_SIZE);
            if (slot == 0U) {
                /* The first slot is the minimum viable receive path. Do not
                 * start a WebSocket session without at least one fixed
                 * message buffer, but retain any future retry opportunity. */
                esp_xiaozhi_websocket_log_incoming_stats(
                    websocket, "receive pool allocation failed");
                while (xQueueReceive(websocket->incoming_free_queue, &stale_slot, 0) == pdTRUE) {
                }
                return false;
            }

            /* The second slot is a throughput reserve. PSRAM can be
             * fragmented by the display/emote assets even when one 32 KiB
             * block remains available, so keep the first slot and run in a
             * deliberately backpressured one-slot mode. */
            ESP_LOGW(TAG,
                     "WebSocket receive pool degraded: slot=%u unavailable; "
                     "continuing with %u slot",
                     (unsigned)slot, (unsigned)allocated_slots);
            esp_xiaozhi_websocket_log_incoming_stats(
                websocket, "receive pool partially allocated");
            break;
        }
        ++allocated_slots;
    }

    if (allocated_slots == 0U) {
        while (xQueueReceive(websocket->incoming_free_queue, &stale_slot, 0) == pdTRUE) {
        }
        return false;
    }

    /* Seed the free-index queue only after all allocation decisions are made.
     * Queue storage is deliberately tiny, but treat a failed seed as a real
     * initialization failure and release every block so the next wake can
     * retry from a clean ownership state. */
    for (uint8_t slot = 0;
            slot < ESP_XIAOZHI_WEBSOCKET_INCOMING_POOL_BLOCK_COUNT; ++slot) {
        if (websocket->incoming_pool[slot] == NULL) {
            continue;
        }
        if (xQueueSend(websocket->incoming_free_queue, &slot, 0) != pdTRUE) {
            ESP_LOGE(TAG,
                     "Failed to seed WebSocket receive pool queue at slot=%u",
                     (unsigned)slot);
            for (uint8_t released = 0;
                    released < ESP_XIAOZHI_WEBSOCKET_INCOMING_POOL_BLOCK_COUNT;
                    ++released) {
                heap_caps_free(websocket->incoming_pool[released]);
                websocket->incoming_pool[released] = NULL;
            }
            websocket->incoming_pool_in_use_mask = 0;
            websocket->incoming_pool_in_use = 0;
            websocket->incoming_pool_ready = false;
            while (xQueueReceive(websocket->incoming_free_queue, &stale_slot, 0) == pdTRUE) {
            }
            esp_xiaozhi_websocket_log_incoming_stats(
                websocket, "receive pool queue seed failed");
            return false;
        }
    }

    websocket->incoming_pool_ready = true;
    esp_xiaozhi_websocket_recount_pool_locked(websocket);

    ESP_LOGI(TAG,
             "WebSocket receive pool ready: slots=%u block=%u bytes "
             "(PSRAM-backed, no per-frame malloc)",
             (unsigned)allocated_slots,
             (unsigned)ESP_XIAOZHI_WEBSOCKET_INCOMING_POOL_BLOCK_SIZE);
    esp_xiaozhi_websocket_log_incoming_stats(websocket, "receive pool ready");
    return true;
}

static void esp_xiaozhi_websocket_deinit_incoming_pool(
    esp_xiaozhi_websocket_t *websocket)
{
    if (websocket == NULL) {
        return;
    }
    for (size_t slot = 0; slot < ESP_XIAOZHI_WEBSOCKET_INCOMING_POOL_BLOCK_COUNT;
            ++slot) {
        heap_caps_free(websocket->incoming_pool[slot]);
        websocket->incoming_pool[slot] = NULL;
    }
    websocket->incoming_pool_in_use_mask = 0;
    websocket->incoming_pool_in_use = 0;
    websocket->incoming_pool_high_watermark = 0;
    websocket->incoming_assembly_slot =
        ESP_XIAOZHI_WEBSOCKET_INCOMING_INVALID_SLOT;
    websocket->incoming_assembly_opcode = 0;
    websocket->incoming_assembly_len = 0;
    websocket->incoming_frame_opcode =
        ESP_XIAOZHI_WEBSOCKET_INCOMING_INVALID_OPCODE;
    websocket->incoming_frame_base = 0;
    websocket->incoming_frame_len = 0;
    websocket->incoming_pool_ready = false;
    if (websocket->incoming_free_queue != NULL) {
        uint8_t stale_slot;
        while (xQueueReceive(websocket->incoming_free_queue, &stale_slot, 0) == pdTRUE) {
        }
    }
}

static void esp_xiaozhi_websocket_deinit_incoming_task_storage(
    esp_xiaozhi_websocket_t *websocket)
{
    if (websocket == NULL) {
        return;
    }

    /* cleanup_session() has already reclaimed the live task.  Only the
     * persistent reserve remains here, so it is safe to return it once when
     * the whole websocket wrapper is finally destroyed. */
    if (websocket->incoming_task_stack_storage != NULL) {
        heap_caps_free(websocket->incoming_task_stack_storage);
        websocket->incoming_task_stack_storage = NULL;
    }
    websocket->incoming_task_stack_storage_psram = false;
}

/* Runs on the IDF socket task. It must not call application/LVGL code or
 * allocate/free heap objects. It only copies into a pool slot and queues it. */
static void esp_xiaozhi_websocket_handle_data(
    esp_xiaozhi_websocket_t *websocket, uint8_t opcode, bool fin,
    const char *data, int data_len, int payload_offset, int payload_len)
{
    if (websocket == NULL || data == NULL || data_len <= 0 ||
            payload_offset < 0 || !websocket->incoming_pool_ready ||
            websocket->incoming_state_mutex == NULL) {
        return;
    }

    if (xSemaphoreTake(websocket->incoming_state_mutex, 0) != pdTRUE) {
        ++websocket->incoming_pool_drops;
        return;
    }
    if (!esp_xiaozhi_websocket_incoming_accepting(websocket)) {
        xSemaphoreGive(websocket->incoming_state_mutex);
        return;
    }

    const size_t offset = (size_t)payload_offset;
    const size_t chunk_len = (size_t)data_len;
    const size_t frame_len = payload_len > 0 ? (size_t)payload_len : 0U;
    const bool is_data_opcode =
        opcode == WS_OPCODE_TEXT || opcode == WS_OPCODE_BINARY;

    if (chunk_len > ESP_XIAOZHI_WEBSOCKET_INCOMING_POOL_BLOCK_SIZE ||
            frame_len > ESP_XIAOZHI_WEBSOCKET_INCOMING_POOL_BLOCK_SIZE) {
        ++websocket->incoming_pool_drops;
        esp_xiaozhi_websocket_reset_assembly_locked(websocket);
        xSemaphoreGive(websocket->incoming_state_mutex);
        return;
    }

    /* payload_offset is relative to the current WebSocket frame. It resets to
     * zero for a continuation frame and advances for a frame that is larger
     * than the transport buffer, so retain a separate frame base. */
    if (offset == 0U) {
        if (is_data_opcode) {
            /* A TEXT/BINARY opcode starts a new message. If an older message
             * was incomplete, discard it rather than mixing the two. */
            if (websocket->incoming_assembly_slot !=
                    ESP_XIAOZHI_WEBSOCKET_INCOMING_INVALID_SLOT) {
                esp_xiaozhi_websocket_reset_assembly_locked(websocket);
            }
            if (!esp_xiaozhi_websocket_take_pool_slot_locked(
                        websocket, &websocket->incoming_assembly_slot)) {
                xSemaphoreGive(websocket->incoming_state_mutex);
                return;
            }
            websocket->incoming_assembly_opcode = opcode;
        } else if (opcode == WS_OPCODE_CONTINUATION) {
            if (websocket->incoming_assembly_slot ==
                        ESP_XIAOZHI_WEBSOCKET_INCOMING_INVALID_SLOT ||
                    websocket->incoming_assembly_opcode == 0) {
                ++websocket->incoming_pool_drops;
                xSemaphoreGive(websocket->incoming_state_mutex);
                return;
            }
        } else {
            xSemaphoreGive(websocket->incoming_state_mutex);
            return;
        }

        websocket->incoming_frame_opcode = opcode;
        /* payload_offset is relative to this frame and already advances for
         * each transport-buffer chunk. Only a CONTINUATION frame starts after
         * the bytes assembled by the previous frame; chunks of the initial
         * TEXT/BINARY frame must keep a zero base. */
        websocket->incoming_frame_base =
            opcode == WS_OPCODE_CONTINUATION
                ? websocket->incoming_assembly_len
                : 0U;
        websocket->incoming_frame_len = frame_len;
    } else if (websocket->incoming_assembly_slot ==
                    ESP_XIAOZHI_WEBSOCKET_INCOMING_INVALID_SLOT ||
            websocket->incoming_frame_opcode ==
                ESP_XIAOZHI_WEBSOCKET_INCOMING_INVALID_OPCODE ||
            websocket->incoming_frame_opcode != opcode) {
        ++websocket->incoming_pool_drops;
        esp_xiaozhi_websocket_reset_assembly_locked(websocket);
        xSemaphoreGive(websocket->incoming_state_mutex);
        return;
    } else if (frame_len > 0U && websocket->incoming_frame_len > 0U &&
            frame_len != websocket->incoming_frame_len) {
        ++websocket->incoming_pool_drops;
        esp_xiaozhi_websocket_reset_assembly_locked(websocket);
        xSemaphoreGive(websocket->incoming_state_mutex);
        return;
    } else if (websocket->incoming_frame_len == 0U && frame_len > 0U) {
        /* Older transports may omit the frame length on the first callback;
         * capture it as soon as a later chunk reports it. */
        websocket->incoming_frame_len = frame_len;
    }

    const size_t expected_frame_len = websocket->incoming_frame_len;
    if ((expected_frame_len > 0U &&
            (offset > expected_frame_len ||
             chunk_len > expected_frame_len - offset)) ||
            offset > ESP_XIAOZHI_WEBSOCKET_INCOMING_POOL_BLOCK_SIZE ||
            chunk_len > ESP_XIAOZHI_WEBSOCKET_INCOMING_POOL_BLOCK_SIZE - offset ||
            websocket->incoming_frame_base >
                ESP_XIAOZHI_WEBSOCKET_INCOMING_POOL_BLOCK_SIZE - offset - chunk_len) {
        ++websocket->incoming_pool_drops;
        esp_xiaozhi_websocket_reset_assembly_locked(websocket);
        xSemaphoreGive(websocket->incoming_state_mutex);
        return;
    }

    const size_t copy_offset = websocket->incoming_frame_base + offset;
    const size_t end = copy_offset + chunk_len;
    memcpy(websocket->incoming_pool[websocket->incoming_assembly_slot] +
               copy_offset,
           data, chunk_len);
    if (end > websocket->incoming_assembly_len) {
        websocket->incoming_assembly_len = end;
    }

    /* A zero payload_len is only an "unknown" length for older transports;
     * in that case FIN, rather than the first chunk, marks completion. */
    const bool frame_complete = expected_frame_len > 0U
        ? offset + chunk_len >= expected_frame_len
        : fin;
    if (frame_complete) {
        if (fin) {
            const uint8_t slot = websocket->incoming_assembly_slot;
            const uint8_t message_opcode = websocket->incoming_assembly_opcode;
            const size_t message_len = websocket->incoming_assembly_len;
            websocket->incoming_assembly_slot =
                ESP_XIAOZHI_WEBSOCKET_INCOMING_INVALID_SLOT;
            websocket->incoming_assembly_opcode = 0;
            websocket->incoming_assembly_len = 0;
            websocket->incoming_frame_opcode =
                ESP_XIAOZHI_WEBSOCKET_INCOMING_INVALID_OPCODE;
            websocket->incoming_frame_base = 0;
            websocket->incoming_frame_len = 0;
            (void)esp_xiaozhi_websocket_queue_pool_slot_locked(
                websocket, message_opcode, slot, message_len);
        } else {
            /* The next frame must be a CONTINUATION. Its base is selected
             * when that frame's first chunk arrives; do not add the current
             * assembly length to later chunks of this same frame because
             * payload_offset is already frame-relative. */
            websocket->incoming_frame_opcode =
                ESP_XIAOZHI_WEBSOCKET_INCOMING_INVALID_OPCODE;
            websocket->incoming_frame_base = 0;
            websocket->incoming_frame_len = 0;
        }
    }
    xSemaphoreGive(websocket->incoming_state_mutex);
}

static BaseType_t esp_xiaozhi_websocket_create_incoming_task(
    esp_xiaozhi_websocket_t *websocket)
{
    const configSTACK_DEPTH_TYPE stack_depth =
        ESP_XIAOZHI_WEBSOCKET_INCOMING_TASK_STACK;

    if (websocket == NULL) {
        return pdFAIL;
    }

    /* Do not erase a task/stack pointer that survived a failed allocator
     * attempt.  The owner cleanup path needs that metadata to reclaim an
     * orphaned task safely on the next retry. */
    if (websocket->incoming_task != NULL ||
            websocket->incoming_task_stack != NULL) {
        ESP_LOGE(TAG,
                 "Receive task allocation exhausted with unreclaimed task metadata "
                 "(task=%p stack=%p); refusing to orphan it",
                 (void *)websocket->incoming_task,
                 (void *)websocket->incoming_task_stack);
        return pdFAIL;
    }
    websocket->incoming_task = NULL;
    websocket->incoming_task_with_caps = false;
    websocket->incoming_task_static = false;
    websocket->incoming_task_stack = NULL;
    __atomic_store_n(&websocket->incoming_task_published, false,
                     __ATOMIC_RELEASE);
    ESP_LOGI(TAG,
             "WebSocket receive task allocator active (internal RAM only; PSRAM reserved for receive pool; stack=%u bytes)",
             (unsigned)esp_xiaozhi_websocket_task_stack_bytes(stack_depth));

    for (unsigned attempt = 1;
            attempt <= ESP_XIAOZHI_WEBSOCKET_INCOMING_TASK_CREATE_MAX_ATTEMPTS;
            ++attempt) {
        if (websocket->incoming_task != NULL ||
                websocket->incoming_task_stack != NULL) {
            ESP_LOGE(TAG,
                     "Receive task retry has unreclaimed metadata (task=%p stack=%p)",
                     (void *)websocket->incoming_task,
                     (void *)websocket->incoming_task_stack);
            return pdFAIL;
        }
        websocket->incoming_task = NULL;
        websocket->incoming_task_with_caps = false;
        websocket->incoming_task_static = false;
        websocket->incoming_task_stack = NULL;
        __atomic_store_n(&websocket->incoming_task_published, false,
                         __ATOMIC_RELEASE);
        /* Prefer the embedded-TCB/static-task path.  Its internal stack is
         * reserved once and reused across reconnects, so a transient heap
         * shortage cannot turn the next wake into ESP_ERR_NO_MEM.  The IDF
         * WithCaps allocator remains a fallback for ports where static task
         * creation is unavailable or rejected. */
        BaseType_t ret = pdFAIL;
#if CONFIG_SPIRAM && (configSUPPORT_STATIC_ALLOCATION == 1)
        ret = esp_xiaozhi_websocket_create_static_task_with_caps(
            websocket, esp_xiaozhi_websocket_incoming_task, "xiaozhi_ws_rx",
            stack_depth, websocket, ESP_XIAOZHI_WEBSOCKET_INCOMING_TASK_PRIORITY,
            &websocket->incoming_task,
            ESP_XIAOZHI_WEBSOCKET_INCOMING_TASK_CORE,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (ret == pdPASS && websocket->incoming_task != NULL) {
            ESP_LOGI(TAG,
                     "WebSocket receive task stack allocated in internal RAM "
                     "(%u bytes, stack=%p)",
                     (unsigned)esp_xiaozhi_websocket_task_stack_bytes(stack_depth),
                     (void *)websocket->incoming_task_stack);
            ESP_LOGI(TAG, "WebSocket receive task uses a reusable static TCB");
            return ret;
        }
        if (ret == pdPASS) {
            ESP_LOGE(TAG,
                     "WebSocket receive task static allocator returned success without a handle");
            ret = pdFAIL;
        }
        if (ret != pdPASS && websocket->incoming_task != NULL) {
            /* Some allocator implementations can publish a task handle
             * before reporting a late creation error. Preserve the matching
             * static deletion mode so stop_incoming_task() can wait for the
             * worker before reusing its embedded TCB/stack. */
            websocket->incoming_task_static = true;
            ESP_LOGW(TAG,
                      "Internal receive task create returned failure after publishing task=%p; reclaiming it",
                     (void *)websocket->incoming_task);
            const esp_err_t orphan_ret =
                esp_xiaozhi_websocket_stop_incoming_task(websocket);
            if (orphan_ret != ESP_OK) {
                return orphan_ret;
            }
        }
#endif

#if CONFIG_SPIRAM
        if (websocket->incoming_task == NULL) {
            ret = esp_xiaozhi_websocket_create_task_with_caps(
                esp_xiaozhi_websocket_incoming_task, "xiaozhi_ws_rx", stack_depth,
                websocket, ESP_XIAOZHI_WEBSOCKET_INCOMING_TASK_PRIORITY,
                &websocket->incoming_task,
                ESP_XIAOZHI_WEBSOCKET_INCOMING_TASK_CORE,
                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            if (ret == pdPASS && websocket->incoming_task != NULL) {
                websocket->incoming_task_with_caps = true;
                ESP_LOGI(TAG,
                         "WebSocket receive task stack allocated in internal RAM "
                         "(%u bytes, task=%p)",
                         (unsigned)esp_xiaozhi_websocket_task_stack_bytes(stack_depth),
                         (void *)websocket->incoming_task);
                return ret;
            }
            if (ret != pdPASS && websocket->incoming_task != NULL) {
                websocket->incoming_task_with_caps = true;
                ESP_LOGW(TAG,
                          "Internal receive task create returned failure after publishing task=%p; reclaiming it",
                         (void *)websocket->incoming_task);
                const esp_err_t orphan_ret =
                    esp_xiaozhi_websocket_stop_incoming_task(websocket);
                if (orphan_ret != ESP_OK) {
                    return orphan_ret;
                }
            }
            if (ret == pdPASS) {
                ESP_LOGE(TAG,
                         "WebSocket receive task allocator returned success without a handle");
                websocket->incoming_task = NULL;
            }
        }
#endif

        ESP_LOGW(TAG,
                 "WebSocket receive task allocation failed (attempt=%u/%u, stack=%u; "
                 "PSRAM free=%u largest=%u; internal free=%u largest=%u)",
                 attempt, ESP_XIAOZHI_WEBSOCKET_INCOMING_TASK_CREATE_MAX_ATTEMPTS,
                 (unsigned)esp_xiaozhi_websocket_task_stack_bytes(stack_depth),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
              (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
              (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

#if configSUPPORT_STATIC_ALLOCATION == 1
        ret = esp_xiaozhi_websocket_create_static_task_with_caps(
            websocket, esp_xiaozhi_websocket_incoming_task, "xiaozhi_ws_rx",
            stack_depth, websocket, ESP_XIAOZHI_WEBSOCKET_INCOMING_TASK_PRIORITY,
            &websocket->incoming_task,
            ESP_XIAOZHI_WEBSOCKET_INCOMING_TASK_CORE,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (ret == pdPASS && websocket->incoming_task != NULL) {
            ESP_LOGI(TAG,
                     "WebSocket receive task stack allocated in internal RAM "
                     "(%u bytes, stack=%p)",
                     (unsigned)esp_xiaozhi_websocket_task_stack_bytes(stack_depth),
                     (void *)websocket->incoming_task_stack);
            ESP_LOGI(TAG, "WebSocket receive task uses a static internal TCB");
            return ret;
        }
        if (ret == pdPASS) {
            ESP_LOGE(TAG,
                     "WebSocket receive task static allocator returned success without a handle");
            ret = pdFAIL;
        }
        if (ret != pdPASS && websocket->incoming_task != NULL) {
            websocket->incoming_task_static = true;
            ESP_LOGW(TAG,
                     "Internal static receive task create returned failure after publishing task=%p; reclaiming it",
                     (void *)websocket->incoming_task);
            const esp_err_t orphan_ret =
                esp_xiaozhi_websocket_stop_incoming_task(websocket);
            if (orphan_ret != ESP_OK) {
                return orphan_ret;
            }
        }

        /* Use the matching WithCaps allocator for the internal fallback too;
         * this makes task destruction synchronous and prevents a deferred idle
         * cleanup from colliding with the next wake. */
        ret = esp_xiaozhi_websocket_create_task_with_caps(
            esp_xiaozhi_websocket_incoming_task, "xiaozhi_ws_rx", stack_depth,
            websocket, ESP_XIAOZHI_WEBSOCKET_INCOMING_TASK_PRIORITY,
            &websocket->incoming_task,
            ESP_XIAOZHI_WEBSOCKET_INCOMING_TASK_CORE,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (ret == pdPASS && websocket->incoming_task != NULL) {
            websocket->incoming_task_with_caps = true;
            ESP_LOGI(TAG,
                     "WebSocket receive task stack allocated in internal RAM "
                     "(%u bytes, task=%p)",
                     (unsigned)esp_xiaozhi_websocket_task_stack_bytes(stack_depth),
                     (void *)websocket->incoming_task);
            return ret;
        }
        if (ret != pdPASS && websocket->incoming_task != NULL) {
            websocket->incoming_task_with_caps = true;
            ESP_LOGW(TAG,
                     "Internal receive task create returned failure after publishing task=%p; reclaiming it",
                     (void *)websocket->incoming_task);
            const esp_err_t orphan_ret =
                esp_xiaozhi_websocket_stop_incoming_task(websocket);
            if (orphan_ret != ESP_OK) {
                return orphan_ret;
            }
        }
        if (ret == pdPASS) {
            ESP_LOGE(TAG,
                     "WebSocket receive task allocator returned success without a handle");
            websocket->incoming_task = NULL;
        }
#else
        ret = xTaskCreatePinnedToCore(
            esp_xiaozhi_websocket_incoming_task, "xiaozhi_ws_rx", stack_depth,
            websocket, ESP_XIAOZHI_WEBSOCKET_INCOMING_TASK_PRIORITY,
            &websocket->incoming_task, ESP_XIAOZHI_WEBSOCKET_INCOMING_TASK_CORE);
        if (ret == pdPASS && websocket->incoming_task != NULL) {
            websocket->incoming_task_with_caps = false;
            ESP_LOGI(TAG,
                     "WebSocket receive task stack allocated in internal RAM "
                     "(%u bytes, task=%p)",
                     (unsigned)esp_xiaozhi_websocket_task_stack_bytes(stack_depth),
                     (void *)websocket->incoming_task);
            return ret;
        }
        if (ret != pdPASS && websocket->incoming_task != NULL) {
            websocket->incoming_task_with_caps = false;
            ESP_LOGW(TAG,
                     "Internal receive task create returned failure after publishing task=%p; reclaiming it",
                     (void *)websocket->incoming_task);
            const esp_err_t orphan_ret =
                esp_xiaozhi_websocket_stop_incoming_task(websocket);
            if (orphan_ret != ESP_OK) {
                return orphan_ret;
            }
        }
        if (ret == pdPASS) {
            ESP_LOGE(TAG,
                     "WebSocket receive task allocator returned success without a handle");
            websocket->incoming_task = NULL;
        }
#endif

        ESP_LOGW(TAG,
                 "Internal receive task allocation failed (attempt=%u/%u; "
                 "PSRAM free=%u largest=%u; internal free=%u largest=%u)",
                 attempt, ESP_XIAOZHI_WEBSOCKET_INCOMING_TASK_CREATE_MAX_ATTEMPTS,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

        if (attempt < ESP_XIAOZHI_WEBSOCKET_INCOMING_TASK_CREATE_MAX_ATTEMPTS) {
            vTaskDelay(pdMS_TO_TICKS(
                ESP_XIAOZHI_WEBSOCKET_INCOMING_TASK_CREATE_RETRY_DELAY_MS));
        }
    }

    if (websocket->incoming_task != NULL ||
            websocket->incoming_task_stack != NULL) {
        ESP_LOGE(TAG,
                 "Receive task allocation exhausted with unreclaimed task metadata "
                 "(task=%p stack=%p); refusing to orphan it",
                 (void *)websocket->incoming_task,
                 (void *)websocket->incoming_task_stack);
        return pdFAIL;
    }
    websocket->incoming_task = NULL;
    websocket->incoming_task_with_caps = false;
    websocket->incoming_task_static = false;
    websocket->incoming_task_stack = NULL;
    __atomic_store_n(&websocket->incoming_task_published, false,
                     __ATOMIC_RELEASE);
    ESP_LOGE(TAG,
             "Receive task allocation exhausted after %u attempts (stack=%u; "
             "PSRAM free=%u largest=%u; internal free=%u largest=%u)",
             ESP_XIAOZHI_WEBSOCKET_INCOMING_TASK_CREATE_MAX_ATTEMPTS,
             (unsigned)esp_xiaozhi_websocket_task_stack_bytes(stack_depth),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
              (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    esp_xiaozhi_websocket_log_incoming_stats(
        websocket, "receive task allocation exhausted");
    return pdFAIL;
}

static void esp_xiaozhi_websocket_dispatch_event(
    esp_xiaozhi_websocket_t *websocket,
    const esp_xiaozhi_websocket_incoming_event_t *event)
{
    if (websocket == NULL || event == NULL ||
            !esp_xiaozhi_websocket_incoming_accepting(websocket)) {
        return;
    }

    switch (event->type) {
    case ESP_XIAOZHI_WEBSOCKET_INCOMING_EVENT_CONNECTED:
        if (websocket->callbacks.on_connected_callback != NULL) {
            websocket->callbacks.on_connected_callback(websocket->transport_handle);
        }
        break;
    case ESP_XIAOZHI_WEBSOCKET_INCOMING_EVENT_DISCONNECTED:
        if (websocket->callbacks.on_disconnected_callback != NULL) {
            websocket->callbacks.on_disconnected_callback(websocket->transport_handle);
        }
        break;
    case ESP_XIAOZHI_WEBSOCKET_INCOMING_EVENT_ERROR:
        if (websocket->callbacks.on_error_callback != NULL) {
            websocket->callbacks.on_error_callback(
                websocket->transport_handle,
                event->error != ESP_OK ? event->error : ESP_FAIL);
        }
        break;
    default:
        break;
    }
}

static void esp_xiaozhi_websocket_incoming_task(void *arg)
{
    esp_xiaozhi_websocket_t *websocket = (esp_xiaozhi_websocket_t *)arg;
    esp_xiaozhi_websocket_incoming_message_t message = {
        .slot = ESP_XIAOZHI_WEBSOCKET_INCOMING_INVALID_SLOT,
    };

    /* xTaskCreate*() may schedule this task before the creator has returned.
     * Do not touch queues or reclaimer metadata until the owner publishes the
     * complete task record. */
    while (!esp_xiaozhi_websocket_incoming_task_is_published(websocket)) {
        if (websocket->incoming_stop_requested) {
            break;
        }
        vTaskDelay(1);
    }
    if (esp_xiaozhi_websocket_incoming_task_is_published(websocket)) {
        for (;;) {
            if (websocket->incoming_stop_requested) {
                break;
            }

            esp_xiaozhi_websocket_incoming_event_t event;
            if (websocket->incoming_event_queue != NULL &&
                    xQueueReceive(websocket->incoming_event_queue, &event, 0) == pdTRUE) {
                if (event.type == ESP_XIAOZHI_WEBSOCKET_INCOMING_EVENT_STOP) {
                    break;
                }
                esp_xiaozhi_websocket_dispatch_event(websocket, &event);
                continue;
            }

            if (xQueueReceive(websocket->incoming_queue, &message,
                              pdMS_TO_TICKS(50)) != pdTRUE) {
                continue;
            }
            if (message.slot == ESP_XIAOZHI_WEBSOCKET_INCOMING_INVALID_SLOT) {
                if (message.opcode == ESP_XIAOZHI_WEBSOCKET_INCOMING_EVENT_STOP) {
                    break;
                }
                /* Stop markers are kept on the event queue. Treat an invalid
                 * data message as a harmless wakeup for compatibility with a
                 * queue left by an older session. */
                esp_xiaozhi_websocket_free_incoming_message(websocket, &message);
                continue;
            }

            uint8_t *data = NULL;
            if (message.slot < ESP_XIAOZHI_WEBSOCKET_INCOMING_POOL_BLOCK_COUNT) {
                data = websocket->incoming_pool[message.slot];
            }
            if (esp_xiaozhi_websocket_incoming_accepting(websocket)) {
                if (data != NULL && message.opcode == WS_OPCODE_TEXT &&
                        websocket->callbacks.on_message_callback != NULL) {
                    websocket->callbacks.on_message_callback(
                        websocket->transport_handle, NULL, 0,
                        (const char *)data, message.len);
                } else if (data != NULL && message.opcode == WS_OPCODE_BINARY &&
                        websocket->callbacks.on_binary_callback != NULL) {
                    websocket->callbacks.on_binary_callback(
                        websocket->transport_handle, data, message.len);
                }
            }
            esp_xiaozhi_websocket_free_incoming_message(websocket, &message);
        }
    }

    esp_xiaozhi_websocket_free_incoming_message(websocket, &message);
    /* Keep incoming_task non-NULL until the owner consumes incoming_exited.
     * Clearing it here creates a small window where stop() can skip the wait
     * while this task is still executing its final cleanup. */
    xSemaphoreGive(websocket->incoming_exited);
    /* The owner deletes the task with the allocator-matching API after it has
     * observed this token. Suspending here prevents a WithCaps stack from
     * being reclaimed by the wrong FreeRTOS path. */
    vTaskSuspend(NULL);
}

static esp_err_t esp_xiaozhi_websocket_start_incoming_task(
    esp_xiaozhi_websocket_t *websocket)
{
    /* A worker can request its own stop from an application callback.  It
     * deliberately remains suspended until an owner reclaims its stack, so a
     * later wake must finish that handoff before publishing a new worker. */
    for (;;) {
        xSemaphoreTake(websocket->incoming_state_mutex, portMAX_DELAY);
        const bool task_running = websocket->incoming_task != NULL;
        const bool task_stopping = websocket->incoming_stop_requested;
        const bool task_published =
            esp_xiaozhi_websocket_incoming_task_is_published(websocket);
        xSemaphoreGive(websocket->incoming_state_mutex);

        if (task_running && !task_stopping) {
            /* A task can be visible while it is still behind the publication
             * gate. Do not open callback admission until its pool is ready;
             * this also makes a retry after a partial start deterministic. */
            if (!websocket->incoming_pool_ready &&
                    !esp_xiaozhi_websocket_init_incoming_pool(websocket)) {
                ESP_LOGE(TAG, "Failed to prepare WebSocket receive pool");
                return ESP_ERR_NO_MEM;
            }
            esp_xiaozhi_websocket_set_incoming_accepting(websocket, true);
            if (!task_published) {
                esp_xiaozhi_websocket_publish_incoming_task(websocket);
            }
            return ESP_OK;
        }

        if (!task_running) {
            break;
        }
        if (!task_stopping) {
            if (!esp_xiaozhi_websocket_init_incoming_pool(websocket)) {
                ESP_LOGE(TAG, "Failed to prepare WebSocket receive pool");
                return ESP_ERR_NO_MEM;
            }
            return ESP_OK;
        }

        ESP_LOGI(TAG,
                 "WebSocket receive task stop is pending; owner reaping before restart");
        const esp_err_t reap_ret =
            esp_xiaozhi_websocket_stop_incoming_task(websocket);
        if (reap_ret != ESP_OK) {
            /* This is a lifecycle handoff failure, not a heap allocation
             * failure. Keep the distinction visible to the retry path. */
            ESP_LOGW(TAG,
                     "WebSocket receive task cleanup deferred (%s); allocation not attempted",
                     esp_err_to_name(reap_ret));
            return reap_ret;
        }
    }

    /* Close the callback admission gate before flushing.  The event queue and
     * this gate are protected by the same lock, so no lifecycle event can be
     * appended between the flush and the worker stop marker. */
    esp_xiaozhi_websocket_set_incoming_accepting(websocket, false);
    xSemaphoreTake(websocket->incoming_state_mutex, portMAX_DELAY);
    esp_xiaozhi_websocket_flush_incoming_locked(websocket);
    websocket->incoming_stop_requested = false;
    __atomic_store_n(&websocket->incoming_task_published, false,
                     __ATOMIC_RELEASE);
    xSemaphoreGive(websocket->incoming_state_mutex);

    /* A previous worker may have signalled its exit before the owner consumed
     * the binary semaphore. Drain stale tokens before publishing the new task,
     * otherwise stop() could return before this task actually exits. */
    while (xSemaphoreTake(websocket->incoming_exited, 0) == pdTRUE) {
    }

    /* Prepare the fixed receive storage before creating/publishing the worker.
     * The worker entry can run as soon as xTaskCreate*() returns, so publishing
     * it before this point creates a real startup race with pool initialization.
     * Once allocated, the pool is intentionally kept across reconnects. */
    if (!esp_xiaozhi_websocket_init_incoming_pool(websocket)) {
        ESP_LOGE(TAG, "Failed to prepare WebSocket receive pool");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ret = esp_xiaozhi_websocket_create_incoming_task(websocket);
    if (ret != pdPASS) {
        /* A failed create normally leaves no task, but use the same stop path
         * when a task was created and failed during owner publication. Keep
         * the fixed pool allocated after a failed attempt so the retry does
         * not create another PSRAM fragmentation cycle. */
        const esp_err_t stop_ret =
            esp_xiaozhi_websocket_stop_incoming_task(websocket);
        ESP_LOGE(TAG,
                 "Failed to create asynchronous WebSocket receive task (%s); "
                 "upstream WebSocket task was not attempted",
                 esp_err_to_name(stop_ret == ESP_OK ? ESP_ERR_NO_MEM : stop_ret));
        return stop_ret == ESP_OK ? ESP_ERR_NO_MEM : stop_ret;
    }

    /* All task metadata and pool pointers are now initialized. Open callback
     * admission first, then release the worker's publication gate. */
    esp_xiaozhi_websocket_set_incoming_accepting(websocket, true);
    esp_xiaozhi_websocket_publish_incoming_task(websocket);
    return ESP_OK;
}

static esp_err_t esp_xiaozhi_websocket_stop_incoming_task(
    esp_xiaozhi_websocket_t *websocket)
{
    if (websocket == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (websocket->incoming_queue == NULL ||
            websocket->incoming_state_mutex == NULL ||
            websocket->incoming_exited == NULL) {
        /* A task cannot normally be started before these objects are created.
         * If an allocator nevertheless published metadata during a partial
         * initialization, do not silently abandon it and let the next wake
         * report a misleading NO_MEM.  There is no safe queue handoff here,
         * so retain the live handle for the caller to retry after init. */
        if (websocket->incoming_task != NULL ||
                websocket->incoming_task_stack != NULL) {
            ESP_LOGE(TAG,
                     "Cannot stop receive task before lifecycle queues exist "
                     "(task=%p stack=%p)",
                     (void *)websocket->incoming_task,
                     (void *)websocket->incoming_task_stack);
            return ESP_ERR_INVALID_STATE;
        }
        websocket->incoming_stop_requested = false;
        __atomic_store_n(&websocket->incoming_task_published, false,
                         __ATOMIC_RELEASE);
        return ESP_OK;
    }

    /* Close admission before taking the state mutex.  This ordering is
     * deliberate: the socket callback never waits on the worker's mutex, and
     * stop() can therefore flush a stable queue snapshot. */
    esp_xiaozhi_websocket_set_incoming_accepting(websocket, false);
    if (!esp_xiaozhi_websocket_wait_event_callbacks_idle(websocket)) {
        ESP_LOGE(TAG,
                 "Timed out waiting for WebSocket event callback handoff; "
                 "leaving receive task metadata for retry");
        return ESP_ERR_TIMEOUT;
    }
    xSemaphoreTake(websocket->incoming_state_mutex, portMAX_DELAY);
    websocket->incoming_stop_requested = true;
    esp_xiaozhi_websocket_flush_incoming_locked(websocket);
    xSemaphoreGive(websocket->incoming_state_mutex);

    TaskHandle_t task = NULL;
    bool task_with_caps = false;
    bool task_static = false;
    StackType_t *task_stack = NULL;

    xSemaphoreTake(websocket->incoming_state_mutex, portMAX_DELAY);
    task = websocket->incoming_task;
    const bool task_running = task != NULL;
    task_with_caps = websocket->incoming_task_with_caps;
    task_static = websocket->incoming_task_static;
    task_stack = websocket->incoming_task_stack;
    xSemaphoreGive(websocket->incoming_state_mutex);
    if (!task_running) {
        /* The worker may have already published its exit token before the
         * owner observed the handle.  Consume any stale token and clear the
         * stop state so the next start is eligible to allocate a worker. */
        xSemaphoreTake(websocket->incoming_state_mutex, portMAX_DELAY);
        /* A static-stack allocation can survive a failed task publication
         * with no handle to reclaim.  Detach all metadata while protected;
         * release the orphaned stack only after leaving the mutex. */
        const bool orphan_static_stack =
            task_static && task_stack != NULL;
        websocket->incoming_stop_requested = false;
        websocket->incoming_task = NULL;
        websocket->incoming_task_with_caps = false;
        websocket->incoming_task_static = false;
        websocket->incoming_task_stack = NULL;
        __atomic_store_n(&websocket->incoming_task_published, false,
                         __ATOMIC_RELEASE);
        esp_xiaozhi_websocket_flush_incoming_locked(websocket);
        xSemaphoreGive(websocket->incoming_state_mutex);
        while (xSemaphoreTake(websocket->incoming_exited, 0) == pdTRUE) {
        }
        if (orphan_static_stack) {
            /* The static stack is a persistent reserve.  Keep it for the next
             * task creation and release it only in websocket_deinit(). */
            ESP_LOGI(TAG,
                     "Retained reusable WebSocket receive task stack (%u bytes)",
                     (unsigned)ESP_XIAOZHI_WEBSOCKET_INCOMING_TASK_STACK);
        }
        return ESP_OK;
    }

    /* A business callback can request transport shutdown synchronously.  The
     * callback runs on this worker, so waiting for incoming_exited here would
     * wait on the current task forever.  Mark the stop and let the worker
     * leave its loop; an owner task must retry the session cleanup afterward. */
    if (task == xTaskGetCurrentTaskHandle()) {
        ESP_LOGW(TAG,
                 "WebSocket receive task requested its own stop; deferring task reclamation");
        return ESP_ERR_INVALID_STATE;
    }

    esp_xiaozhi_websocket_incoming_event_t stop_event = {
        .type = ESP_XIAOZHI_WEBSOCKET_INCOMING_EVENT_STOP,
        .error = ESP_OK,
    };
    if (websocket->incoming_event_queue != NULL) {
        if (xQueueSend(websocket->incoming_event_queue, &stop_event,
                       pdMS_TO_TICKS(100)) != pdTRUE) {
            ESP_LOGW(TAG, "Failed to queue WebSocket receive task stop event");
        }
    } else {
        esp_xiaozhi_websocket_incoming_message_t stop_message = {
            .opcode = ESP_XIAOZHI_WEBSOCKET_INCOMING_EVENT_STOP,
            .slot = ESP_XIAOZHI_WEBSOCKET_INCOMING_INVALID_SLOT,
        };
        if (xQueueSend(websocket->incoming_queue, &stop_message,
                       pdMS_TO_TICKS(100)) != pdTRUE) {
            ESP_LOGW(TAG, "Failed to queue WebSocket receive task stop message");
        }
    }
    if (xSemaphoreTake(websocket->incoming_exited,
                       pdMS_TO_TICKS(ESP_XIAOZHI_WEBSOCKET_INCOMING_TASK_EXIT_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG,
                 "Timed out waiting for WebSocket receive task to exit; "
                 "leaving task metadata for a later cleanup retry");
        return ESP_ERR_TIMEOUT;
    }

    if (task != NULL && task != xTaskGetCurrentTaskHandle()) {
        /* The worker suspends itself immediately after publishing the exit
         * token. Suspend once more from the owner to close the tiny handoff
         * race, then use the allocator-matching deletion API. */
        vTaskSuspend(task);
#if configSUPPORT_STATIC_ALLOCATION == 1
        if (task_static) {
            /* Static tasks do not go through vTaskDeleteWithCaps(). Wait for
             * the scheduler on every core to stop using the task stack before
             * returning the PSRAM block to the heap. */
            esp_xiaozhi_websocket_wait_task_not_running(task);
            vTaskDelete(task);
        } else if (task_with_caps) {
            vTaskDeleteWithCaps(task);
        } else {
            vTaskDelete(task);
        }
#else
        vTaskDelete(task);
#endif
        /* Keep a static stack reserve attached to the websocket object.  It is
         * released by esp_xiaozhi_websocket_deinit() after the final session. */
        ESP_LOGI(TAG, "WebSocket receive task reclaimed (%s stack, %s TCB)",
                 task_static ? "static-caps" :
                     (task_with_caps ? "caps" : "internal"),
                 task_static ? "static" : "dynamic");
    }

    xSemaphoreTake(websocket->incoming_state_mutex, portMAX_DELAY);
    if (websocket->incoming_task == task) {
        websocket->incoming_task = NULL;
        websocket->incoming_task_with_caps = false;
        websocket->incoming_task_static = false;
        websocket->incoming_task_stack = NULL;
    }
    __atomic_store_n(&websocket->incoming_task_published, false,
                     __ATOMIC_RELEASE);
    websocket->incoming_stop_requested = false;
    esp_xiaozhi_websocket_flush_incoming_locked(websocket);
    xSemaphoreGive(websocket->incoming_state_mutex);
    esp_xiaozhi_websocket_log_incoming_stats(websocket, "receive task reclaimed");
    return ESP_OK;
}

static void esp_xiaozhi_websocket_notify_disconnected(
    esp_xiaozhi_websocket_t *websocket)
{
    websocket->websocket_connected = false;
    if (websocket->lifecycle_events != NULL) {
        xEventGroupClearBits(websocket->lifecycle_events,
                             ESP_XIAOZHI_WEBSOCKET_CONNECTED_BIT);
    }
    if (websocket->disconnect_notified) {
        return;
    }

    websocket->disconnect_notified = true;
    (void)esp_xiaozhi_websocket_queue_event(
        websocket, ESP_XIAOZHI_WEBSOCKET_INCOMING_EVENT_DISCONNECTED, ESP_OK);
}

static esp_err_t esp_xiaozhi_websocket_map_missing_config_error(esp_err_t err, const char *field_name)
{
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "Required WebSocket config '%s' is missing", field_name);
        return ESP_ERR_NOT_FOUND;
    }
    return err;
}

static void esp_xiaozhi_websocket_build_headers(esp_xiaozhi_websocket_t *websocket)
{
    esp_xiaozhi_chat_board_info_t board_info = {0};
    esp_err_t ret = esp_xiaozhi_chat_get_board_info(&board_info);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get board info: %s", esp_err_to_name(ret));
    }

    websocket->headers[0] = '\0';
    if (strlen(websocket->token) > 0) {
        if (strchr(websocket->token, ' ') == NULL) {
            snprintf(websocket->headers + strlen(websocket->headers),
                     sizeof(websocket->headers) - strlen(websocket->headers),
                     "Authorization: Bearer %s\r\n", websocket->token);
        } else {
            snprintf(websocket->headers + strlen(websocket->headers),
                     sizeof(websocket->headers) - strlen(websocket->headers),
                     "Authorization: %s\r\n", websocket->token);
        }
    }

    snprintf(websocket->headers + strlen(websocket->headers),
             sizeof(websocket->headers) - strlen(websocket->headers),
             "Protocol-Version: %s\r\n", websocket->protocol_version);

    if (board_info.initialized) {
        snprintf(websocket->headers + strlen(websocket->headers),
                 sizeof(websocket->headers) - strlen(websocket->headers),
                 "Device-Id: %s\r\nClient-Id: %s\r\n",
                 board_info.mac_address, board_info.uuid);
    }
}

static void esp_xiaozhi_websocket_event(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)base;
    esp_xiaozhi_websocket_t *websocket = (esp_xiaozhi_websocket_t *)handler_args;
    esp_websocket_event_data_t *event = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGD(TAG, "WEBSOCKET_EVENT_CONNECTED");
        websocket->websocket_connected = true;
        websocket->finished = false;
        websocket->disconnect_notified = false;
        if (websocket->lifecycle_events != NULL) {
            xEventGroupSetBits(websocket->lifecycle_events,
                               ESP_XIAOZHI_WEBSOCKET_CONNECTED_BIT);
        }
        (void)esp_xiaozhi_websocket_queue_event(
            websocket, ESP_XIAOZHI_WEBSOCKET_INCOMING_EVENT_CONNECTED, ESP_OK);
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGD(TAG, "WEBSOCKET_EVENT_DISCONNECTED");
        esp_xiaozhi_websocket_notify_disconnected(websocket);
        break;

    case WEBSOCKET_EVENT_CLOSED:
        ESP_LOGD(TAG, "WEBSOCKET_EVENT_CLOSED");
        esp_xiaozhi_websocket_notify_disconnected(websocket);
        break;

    case WEBSOCKET_EVENT_FINISH:
        ESP_LOGD(TAG, "WEBSOCKET_EVENT_FINISH");
        /* FINISH is emitted immediately before the IDF websocket task sets
         * STOPPED_BIT and reaches the wrapper's owner-reclaim handoff. Keep
         * connection_started asserted until cleanup has reclaimed that task. */
        websocket->finished = true;
        esp_xiaozhi_websocket_notify_disconnected(websocket);
        if (websocket->lifecycle_events != NULL) {
            xEventGroupSetBits(websocket->lifecycle_events,
                               ESP_XIAOZHI_WEBSOCKET_FINISHED_BIT);
        }
        break;

    case WEBSOCKET_EVENT_DATA: {
        /* The socket callback only copies into the fixed pool and queues a
         * slot. JSON/MCP/LVGL work is intentionally deferred to the receive
         * worker so no application callback runs under the IDF client lock. */
        uint8_t op = (uint8_t)event->op_code;
        bool fin = event->fin;
        const char *data = event->data_ptr;
        int data_len = event->data_len;
        int payload_offset = event->payload_offset;
        int payload_len = event->payload_len;

        if (op != WS_OPCODE_TEXT && op != WS_OPCODE_BINARY && op != WS_OPCODE_CONTINUATION) {
            ESP_LOGD(TAG, "Ignore websocket opcode=%d len=%d", op, data_len);
            break;
        }
        if (!data || data_len <= 0) {
            break;
        }

        esp_xiaozhi_websocket_handle_data(
            websocket, op, fin, data, data_len, payload_offset, payload_len);
        break;
    }

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WEBSOCKET_EVENT_ERROR");
        (void)esp_xiaozhi_websocket_queue_event(
            websocket, ESP_XIAOZHI_WEBSOCKET_INCOMING_EVENT_ERROR, ESP_FAIL);
        break;

    default:
        ESP_LOGD(TAG, "Other websocket event id: %" PRId32, event_id);
        break;
    }
}

esp_err_t esp_xiaozhi_websocket_init(esp_xiaozhi_websocket_handle_t *handle)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "Invalid handle");

    /* The receive worker owns a StaticTask_t embedded in this context.  Keep
     * the whole context in internal 8-bit RAM so FreeRTOS never receives a
     * PSRAM TCB, even when the project allows large allocations to spill into
     * external RAM. */
    esp_xiaozhi_websocket_t *websocket = heap_caps_calloc(
        1, sizeof(esp_xiaozhi_websocket_t),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(websocket, ESP_ERR_NO_MEM, TAG, "Failed to allocate memory for websocket");
    websocket->incoming_assembly_slot =
        ESP_XIAOZHI_WEBSOCKET_INCOMING_INVALID_SLOT;
    websocket->incoming_frame_opcode =
        ESP_XIAOZHI_WEBSOCKET_INCOMING_INVALID_OPCODE;

    websocket->lifecycle_events = xEventGroupCreate();
    if (websocket->lifecycle_events == NULL) {
        heap_caps_free(websocket);
        return ESP_ERR_NO_MEM;
    }
    websocket->lifecycle_mutex = xSemaphoreCreateMutex();
    if (websocket->lifecycle_mutex == NULL) {
        vEventGroupDelete(websocket->lifecycle_events);
        heap_caps_free(websocket);
        return ESP_ERR_NO_MEM;
    }
    websocket->incoming_queue = xQueueCreate(
        ESP_XIAOZHI_WEBSOCKET_INCOMING_QUEUE_DEPTH,
        sizeof(esp_xiaozhi_websocket_incoming_message_t));
    websocket->incoming_event_queue = xQueueCreate(
        ESP_XIAOZHI_WEBSOCKET_INCOMING_EVENT_QUEUE_DEPTH,
        sizeof(esp_xiaozhi_websocket_incoming_event_t));
    websocket->incoming_free_queue = xQueueCreate(
        ESP_XIAOZHI_WEBSOCKET_INCOMING_POOL_BLOCK_COUNT, sizeof(uint8_t));
    websocket->incoming_state_mutex = xSemaphoreCreateMutex();
    websocket->incoming_exited = xSemaphoreCreateBinary();
    if (websocket->incoming_queue == NULL ||
            websocket->incoming_event_queue == NULL ||
            websocket->incoming_free_queue == NULL ||
            websocket->incoming_state_mutex == NULL ||
            websocket->incoming_exited == NULL) {
        if (websocket->incoming_queue != NULL) {
            vQueueDelete(websocket->incoming_queue);
        }
        if (websocket->incoming_event_queue != NULL) {
            vQueueDelete(websocket->incoming_event_queue);
        }
        if (websocket->incoming_free_queue != NULL) {
            vQueueDelete(websocket->incoming_free_queue);
        }
        if (websocket->incoming_state_mutex != NULL) {
            vSemaphoreDelete(websocket->incoming_state_mutex);
        }
        if (websocket->incoming_exited != NULL) {
            vSemaphoreDelete(websocket->incoming_exited);
        }
        vSemaphoreDelete(websocket->lifecycle_mutex);
        vEventGroupDelete(websocket->lifecycle_events);
        heap_caps_free(websocket);
        return ESP_ERR_NO_MEM;
    }

    *handle = (esp_xiaozhi_websocket_handle_t)websocket;
    websocket->disconnect_notified = true;

    return ESP_OK;
}

static bool esp_xiaozhi_websocket_client_is_connected(
    const esp_xiaozhi_websocket_t *websocket)
{
    return websocket != NULL && websocket->websocket_client != NULL &&
           websocket->websocket_connected &&
           esp_websocket_client_is_connected(websocket->websocket_client);
}

static esp_err_t esp_xiaozhi_websocket_lock(
    esp_xiaozhi_websocket_t *websocket)
{
    if (websocket == NULL || websocket->lifecycle_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return xSemaphoreTake(websocket->lifecycle_mutex, portMAX_DELAY) == pdTRUE
               ? ESP_OK
               : ESP_ERR_TIMEOUT;
}

static void esp_xiaozhi_websocket_reset_session_state(
    esp_xiaozhi_websocket_t *websocket)
{
    websocket->websocket_connected = false;
    websocket->connection_started = false;
    websocket->finished = false;
    websocket->disconnect_notified = true;
    if (websocket->lifecycle_events != NULL) {
        xEventGroupClearBits(websocket->lifecycle_events,
                             ESP_XIAOZHI_WEBSOCKET_CONNECTED_BIT |
                                 ESP_XIAOZHI_WEBSOCKET_FINISHED_BIT);
    }
}

/* Called with lifecycle_mutex held, after the IDF WebSocket task has stopped.
 * Keeping destruction in one locked helper makes it impossible for a new
 * session to observe a half-destroyed client. */
static esp_err_t websocket_cleanup_locked(
    esp_xiaozhi_websocket_t *websocket)
{
    if (websocket->websocket_client == NULL) {
        websocket->stopping = false;
        esp_xiaozhi_websocket_reset_session_state(websocket);
        return ESP_OK;
    }

    esp_websocket_client_handle_t client = websocket->websocket_client;
    /* Reclaim the wrapper-owned task/stack while the opaque client is still
     * valid.  Destroying first can free client->status_bits and leave a task
     * handle that cannot be safely inspected by the allocator wrapper. */
    const esp_err_t reclaim_ret =
        esp_websocket_client_psram_reclaim_task(client);
    if (reclaim_ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to reclaim WebSocket task before destroy: %s",
                 esp_err_to_name(reclaim_ret));
        websocket->stopping = false;
        return reclaim_ret;
    }

    const esp_err_t ret = esp_websocket_client_destroy(client);
    if (ret == ESP_OK) {
        websocket->websocket_client = NULL;
        websocket->stopping = false;
        esp_xiaozhi_websocket_reset_session_state(websocket);
    } else {
        /* Keep the handle for a later retry; clearing it here would orphan a
         * client whose non-task resources were not released. */
        websocket->stopping = false;
    }
    return ret;
}

/* A stop/deinit can be requested by more than one app task (for example, a
 * transport error and the wake retry path can overlap). Wait for the owner of
 * the first cleanup to finish instead of returning INVALID_STATE and letting
 * the transport layer free the still-active client. The caller receives the
 * lifecycle mutex held. */
static esp_err_t esp_xiaozhi_websocket_lock_after_cleanup(
    esp_xiaozhi_websocket_t *websocket)
{
    for (;;) {
        esp_err_t ret = esp_xiaozhi_websocket_lock(websocket);
        if (ret != ESP_OK) {
            return ret;
        }
        if (!websocket->stopping) {
            return ESP_OK;
        }
        /* The incoming worker may be in an MCP callback while another task
         * tears the session down.  transport_send_text() calls start() before
         * sending, so waiting here would deadlock: this worker waits for the
         * cleanup owner while the owner waits for this worker to exit. */
        if (websocket->incoming_task == xTaskGetCurrentTaskHandle()) {
            xSemaphoreGive(websocket->lifecycle_mutex);
            ESP_LOGW(TAG,
                     "WebSocket start rejected from receive worker during cleanup");
            return ESP_ERR_INVALID_STATE;
        }
        xSemaphoreGive(websocket->lifecycle_mutex);
        vTaskDelay(1);
    }
}

/* Stop and destroy one complete WebSocket conversation. The wrapper mutex is
 * released while the IDF client task exits so an incoming callback cannot
 * deadlock the stop wait by trying to send an MCP response. */
static esp_err_t esp_xiaozhi_websocket_cleanup_session(
    esp_xiaozhi_websocket_t *websocket)
{
    ESP_RETURN_ON_FALSE(websocket != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "Invalid websocket");

    esp_err_t ret = esp_xiaozhi_websocket_lock_after_cleanup(websocket);
    ESP_RETURN_ON_ERROR(ret, TAG, "Failed to lock websocket lifecycle");

    if (websocket->websocket_client == NULL) {
        websocket->stopping = true;
        xSemaphoreGive(websocket->lifecycle_mutex);
        const esp_err_t incoming_ret =
            esp_xiaozhi_websocket_stop_incoming_task(websocket);
        if (incoming_ret != ESP_OK) {
            if (esp_xiaozhi_websocket_lock(websocket) == ESP_OK) {
                websocket->stopping = false;
                xSemaphoreGive(websocket->lifecycle_mutex);
            }
            return incoming_ret;
        }
        /* Keep the fixed receive pool attached to this websocket object.  A
         * later wake reuses the same slots instead of fragmenting PSRAM with a
         * free/allocate cycle after every failed conversation. */
        if (esp_xiaozhi_websocket_lock(websocket) != ESP_OK) {
            return ESP_ERR_TIMEOUT;
        }
        esp_xiaozhi_websocket_reset_session_state(websocket);
        websocket->stopping = false;
        xSemaphoreGive(websocket->lifecycle_mutex);
        return ESP_OK;
    }

    websocket->stopping = true;
    esp_websocket_client_handle_t client = websocket->websocket_client;
    const bool started = websocket->connection_started;
    xSemaphoreGive(websocket->lifecycle_mutex);

    /* Stop application dispatch before destroying the transport. The worker
     * may still be inside an MCP handler; waiting here prevents a callback
     * from touching the wrapper after the client/session is freed. */
    const esp_err_t incoming_ret =
        esp_xiaozhi_websocket_stop_incoming_task(websocket);
    if (incoming_ret != ESP_OK) {
        if (esp_xiaozhi_websocket_lock(websocket) == ESP_OK) {
            websocket->stopping = false;
            xSemaphoreGive(websocket->lifecycle_mutex);
        }
        return incoming_ret;
    }

    if (started) {
        ret = esp_websocket_client_stop(client);
        /* IDF returns ESP_FAIL when FINISH already set STOPPED_BIT. The task
         * is then complete and destroying the client is still correct. */
        if (ret != ESP_OK && !websocket->finished) {
            ESP_LOGW(TAG, "Failed to stop websocket client: %s",
                     esp_err_to_name(ret));
        } else if (ret != ESP_OK) {
            ret = ESP_OK;
        }
    }

    if (esp_xiaozhi_websocket_lock(websocket) != ESP_OK) {
        /* Keep a future stop from spinning forever if the mutex was
         * temporarily unavailable during an abnormal teardown. */
        websocket->stopping = false;
        return ret != ESP_OK ? ret : ESP_ERR_TIMEOUT;
    }
    if (websocket->websocket_client != client) {
        websocket->stopping = false;
        xSemaphoreGive(websocket->lifecycle_mutex);
        return ret != ESP_OK ? ret : ESP_ERR_INVALID_STATE;
    }
    const esp_err_t destroy_ret = websocket_cleanup_locked(websocket);
    /* Once destroy() has completed, the session is gone even if the IDF stop
     * call reported that it was already stopped. Surface only a real destroy
     * failure so the transport layer can retry deinit without discarding the
     * live handle. */
    ret = destroy_ret;
    xSemaphoreGive(websocket->lifecycle_mutex);
    return ret;
}

static esp_err_t esp_xiaozhi_websocket_send_failed(
    esp_xiaozhi_websocket_t *websocket, int sent, size_t expected,
    const char *frame_type)
{
    const bool client_connected =
        websocket->websocket_client != NULL &&
        esp_websocket_client_is_connected(websocket->websocket_client);

    /* A negative/short write is a transport failure even when the IDF client
     * still reports the socket as connected. Keeping that stale session alive
     * lets the recorder continue producing frames until the audio queue fills,
     * and prevents the next wake from creating a clean conversation. The app
     * owns the retry/reconnect boundary, so invalidate this session now. */
    esp_xiaozhi_websocket_notify_disconnected(websocket);
    ESP_LOGE(TAG,
             "Failed to send %s frame (%d/%u bytes, client_connected=%d); "
             "invalidating session",
             frame_type, sent, (unsigned)expected, client_connected ? 1 : 0);
    return ESP_ERR_INVALID_STATE;
}

esp_err_t esp_xiaozhi_websocket_deinit(esp_xiaozhi_websocket_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "Invalid handle");

    esp_xiaozhi_websocket_t *websocket = (esp_xiaozhi_websocket_t *)handle;
    esp_err_t ret = esp_xiaozhi_websocket_cleanup_session(websocket);
    if (ret != ESP_OK) {
        /* Never release the wrapper or its pool while a concurrent stop still
         * owns the client/task. The caller can retry deinit after that stop
         * completes; freeing here would turn a transient lifecycle race into
         * a use-after-free in the socket callback. */
        ESP_LOGW(TAG, "WebSocket deinit deferred until session cleanup succeeds: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    /* cleanup_session() has stopped the dispatch task, so no callback can
     * reference a pool slot while the fixed buffers are released. */
    esp_xiaozhi_websocket_deinit_incoming_pool(websocket);
    esp_xiaozhi_websocket_deinit_incoming_task_storage(websocket);

    if (websocket->lifecycle_events != NULL) {
        vEventGroupDelete(websocket->lifecycle_events);
        websocket->lifecycle_events = NULL;
    }
    if (websocket->lifecycle_mutex != NULL) {
        vSemaphoreDelete(websocket->lifecycle_mutex);
        websocket->lifecycle_mutex = NULL;
    }
    if (websocket->incoming_queue != NULL) {
        vQueueDelete(websocket->incoming_queue);
        websocket->incoming_queue = NULL;
    }
    if (websocket->incoming_event_queue != NULL) {
        vQueueDelete(websocket->incoming_event_queue);
        websocket->incoming_event_queue = NULL;
    }
    if (websocket->incoming_free_queue != NULL) {
        vQueueDelete(websocket->incoming_free_queue);
        websocket->incoming_free_queue = NULL;
    }
    if (websocket->incoming_state_mutex != NULL) {
        vSemaphoreDelete(websocket->incoming_state_mutex);
        websocket->incoming_state_mutex = NULL;
    }
    if (websocket->incoming_exited != NULL) {
        vSemaphoreDelete(websocket->incoming_exited);
        websocket->incoming_exited = NULL;
    }

    const esp_err_t storage_ret =
        esp_websocket_client_psram_release_task_storage();
    if (storage_ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "WebSocket task stack reserve remains owned after deinit: %s",
                 esp_err_to_name(storage_ret));
    }

    heap_caps_free(websocket);
    return ret;
}

esp_err_t esp_xiaozhi_websocket_start(esp_xiaozhi_websocket_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "Invalid handle");

    esp_xiaozhi_websocket_t *websocket = (esp_xiaozhi_websocket_t *)handle;
    esp_err_t ret = esp_xiaozhi_websocket_lock_after_cleanup(websocket);
    ESP_RETURN_ON_ERROR(ret, TAG, "Failed to lock websocket lifecycle");

    esp_xiaozhi_chat_keystore_t xiaozhi_chat_keystore;
    ret = esp_xiaozhi_chat_keystore_init(&xiaozhi_chat_keystore, "websocket", false);
    ret = esp_xiaozhi_websocket_map_missing_config_error(ret, "namespace");
    if (ret != ESP_OK) {
        xSemaphoreGive(websocket->lifecycle_mutex);
        ESP_LOGE(TAG, "Failed to initialize keystore: %s", esp_err_to_name(ret));
        return ret;
    }

    int protocol_version = 1;
    esp_xiaozhi_chat_keystore_get_string(&xiaozhi_chat_keystore, "url", "", websocket->url, sizeof(websocket->url));
    esp_xiaozhi_chat_keystore_get_string(&xiaozhi_chat_keystore, "token", "", websocket->token, sizeof(websocket->token));
    esp_xiaozhi_chat_keystore_get_int(&xiaozhi_chat_keystore, "version", 1, (int32_t *)&protocol_version);
    esp_xiaozhi_chat_keystore_deinit(&xiaozhi_chat_keystore);

    if (strlen(websocket->url) == 0) {
        xSemaphoreGive(websocket->lifecycle_mutex);
        ESP_LOGE(TAG, "Websocket url is not specified");
        return ESP_ERR_NOT_FOUND;
    }

    if (websocket->websocket_client != NULL && websocket->connection_started) {
        if (websocket->websocket_connected) {
            xSemaphoreGive(websocket->lifecycle_mutex);
            return ESP_OK;
        }
        xSemaphoreGive(websocket->lifecycle_mutex);
        EventBits_t bits = xEventGroupWaitBits(
            websocket->lifecycle_events,
            ESP_XIAOZHI_WEBSOCKET_CONNECTED_BIT |
                ESP_XIAOZHI_WEBSOCKET_FINISHED_BIT,
            pdFALSE, pdFALSE,
            pdMS_TO_TICKS(ESP_XIAOZHI_WEBSOCKET_CONNECT_TIMEOUT_MS));
        if ((bits & ESP_XIAOZHI_WEBSOCKET_CONNECTED_BIT) != 0 &&
                websocket->websocket_connected) {
            return ESP_OK;
        }
        ret = esp_xiaozhi_websocket_cleanup_session(websocket);
        if (ret != ESP_OK) {
            return ret;
        }
        ret = esp_xiaozhi_websocket_lock(websocket);
        if (ret != ESP_OK) {
            return ret;
        }
    } else if (websocket->websocket_client != NULL) {
        xSemaphoreGive(websocket->lifecycle_mutex);
        ret = esp_xiaozhi_websocket_cleanup_session(websocket);
        if (ret != ESP_OK) {
            return ret;
        }
        ret = esp_xiaozhi_websocket_lock(websocket);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    /* Observability: log the WS endpoint (uri contains no credentials; token is
     * sent via header, not logged) so a TLS failure can be traced to the host. */
    ESP_LOGI(TAG, "WebSocket endpoint: %s (protocol v%d)", websocket->url, protocol_version);

    snprintf(websocket->protocol_version, sizeof(websocket->protocol_version), "%d", protocol_version);
    esp_xiaozhi_websocket_build_headers(websocket);

    esp_websocket_client_config_t websocket_cfg = {
        .uri = websocket->url,
        .headers = websocket->headers,
        .crt_bundle_attach = esp_crt_bundle_attach,
        /* Match the official XiaoZhi lifecycle: one socket per conversation.
         * Reconnect is requested by the next wake after the old task has
         * finished, so failed TLS attempts cannot accumulate behind an
         * auto-reconnect loop. */
        .disable_auto_reconnect = true,
        .enable_close_reconnect = false,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms = 10000,
        /* MCP camera capture and vision explanation are offloaded to their
         * worker, so keep the transport task at the official 4 KiB default. */
        .task_stack = 4 * 1024,
        /* LVGL is pinned to CPU1 and emote/audio work is pinned to CPU0. Keep
         * the socket task on CPU0 so TLS polling cannot starve the LVGL task. */
        .task_core_id_set = true,
        .task_core_id = 0,
        /* Keep the transport task at a medium priority. The socket callback
         * only copies/queues now, so it does not need to outrank AFE or the
         * emote renderer while TLS and JSON work run elsewhere. */
        /* Emote/LVGL rendering uses priority 5 on CPU0, but a wedged renderer
         * must not starve the socket (that deafens the whole conversation),
         * so the transport sits one step above it. TLS handshakes are short
         * and the socket blocks on I/O, so the face still gets CPU time. */
        .task_prio = 6,
    };

    if (websocket->websocket_client == NULL) {
        websocket->websocket_client = esp_websocket_client_init(&websocket_cfg);
        if (websocket->websocket_client == NULL) {
            xSemaphoreGive(websocket->lifecycle_mutex);
            ESP_LOGE(TAG, "Failed to initialize websocket client");
            return ESP_ERR_NO_MEM;
        }

        ret = esp_websocket_register_events(websocket->websocket_client, WEBSOCKET_EVENT_ANY, esp_xiaozhi_websocket_event, websocket);
        if (ret != ESP_OK) {
            /* Keep every client destruction on the same wrapper path.  The
             * transport task normally does not exist yet here, but reclaiming
             * first also clears a failed-create owner reservation left by a
             * previous wake before the opaque client is freed. */
            const esp_err_t reclaim_ret =
                esp_websocket_client_psram_reclaim_task(websocket->websocket_client);
            if (reclaim_ret != ESP_OK) {
                ESP_LOGW(TAG,
                         "Failed to reclaim WebSocket task after event registration error: %s",
                         esp_err_to_name(reclaim_ret));
                /* Do not destroy a client while the wrapper still owns a
                 * possibly suspended transport task. Keep the handle for the next
                 * lifecycle cleanup attempt; freeing it here would strand the
                 * global owner record and make later wakes fail permanently. */
                xSemaphoreGive(websocket->lifecycle_mutex);
                return reclaim_ret;
            }
            esp_websocket_client_destroy(websocket->websocket_client);
            websocket->websocket_client = NULL;
            xSemaphoreGive(websocket->lifecycle_mutex);
            return ret;
        }
    }

    ret = esp_xiaozhi_websocket_start_incoming_task(websocket);
    if (ret != ESP_OK) {
        xSemaphoreGive(websocket->lifecycle_mutex);
        (void)esp_xiaozhi_websocket_cleanup_session(websocket);
        return ret;
    }

    xEventGroupClearBits(websocket->lifecycle_events,
                         ESP_XIAOZHI_WEBSOCKET_CONNECTED_BIT |
                             ESP_XIAOZHI_WEBSOCKET_FINISHED_BIT);
    websocket->websocket_connected = false;
    websocket->finished = false;
    websocket->disconnect_notified = true;

    /* esp_websocket_client_start() sets STOPPED_BIT when xTaskCreate fails.
     * Do not mark the wrapper as started until it returns successfully, or the
     * cleanup path would call stop() on a client that never had a task. */
    ret = esp_websocket_client_start(websocket->websocket_client);
    if (ret) {
        websocket->connection_started = false;
        ESP_LOGE(TAG, "Failed to start websocket client: %s", esp_err_to_name(ret));
        xSemaphoreGive(websocket->lifecycle_mutex);
        (void)esp_xiaozhi_websocket_cleanup_session(websocket);
        return ret;
    }
    websocket->connection_started = true;

    /* Do not hold lifecycle_mutex while waiting for the socket task.  The
     * connected event is dispatched by the receive worker, and the first
     * hello/audio send may need the same mutex once the event is observed. */
    xSemaphoreGive(websocket->lifecycle_mutex);
    EventBits_t bits = xEventGroupWaitBits(
        websocket->lifecycle_events,
        ESP_XIAOZHI_WEBSOCKET_CONNECTED_BIT |
            ESP_XIAOZHI_WEBSOCKET_FINISHED_BIT,
        pdTRUE, pdFALSE,
        pdMS_TO_TICKS(ESP_XIAOZHI_WEBSOCKET_CONNECT_TIMEOUT_MS));
    if ((bits & ESP_XIAOZHI_WEBSOCKET_CONNECTED_BIT) != 0 &&
            websocket->websocket_connected) {
        return ESP_OK;
    }

    const esp_err_t connect_ret =
        (bits & ESP_XIAOZHI_WEBSOCKET_FINISHED_BIT) != 0
            ? ESP_FAIL
            : ESP_ERR_TIMEOUT;
    (void)esp_xiaozhi_websocket_cleanup_session(websocket);
    return connect_ret;

}

esp_err_t esp_xiaozhi_websocket_stop(esp_xiaozhi_websocket_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "Invalid handle");

    esp_xiaozhi_websocket_t *websocket = (esp_xiaozhi_websocket_t *)handle;
    return esp_xiaozhi_websocket_cleanup_session(websocket);
}

esp_err_t esp_xiaozhi_websocket_send_text(esp_xiaozhi_websocket_handle_t handle, const char *text)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "Invalid handle");
    ESP_RETURN_ON_FALSE(text, ESP_ERR_INVALID_ARG, TAG, "Invalid text");

    esp_xiaozhi_websocket_t *websocket = (esp_xiaozhi_websocket_t *)handle;
    esp_err_t ret = esp_xiaozhi_websocket_lock(websocket);
    ESP_RETURN_ON_ERROR(ret, TAG, "Failed to lock websocket lifecycle");
    if (websocket->stopping || websocket->websocket_client == NULL) {
        xSemaphoreGive(websocket->lifecycle_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    if (!esp_xiaozhi_websocket_client_is_connected(websocket)) {
        if (websocket->websocket_client != NULL &&
                !esp_websocket_client_is_connected(websocket->websocket_client)) {
            esp_xiaozhi_websocket_notify_disconnected(websocket);
        }
        xSemaphoreGive(websocket->lifecycle_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    const size_t text_len = strlen(text);
    int sent = esp_websocket_client_send_text(
        websocket->websocket_client, text, text_len,
        pdMS_TO_TICKS(ESP_XIAOZHI_WEBSOCKET_TEXT_SEND_TIMEOUT_MS));
    if (sent < 0 || (size_t)sent != text_len) {
        ret = esp_xiaozhi_websocket_send_failed(
            websocket, sent, text_len, "text");
    } else {
        ret = ESP_OK;
    }
    xSemaphoreGive(websocket->lifecycle_mutex);
    return ret;
}

esp_err_t esp_xiaozhi_websocket_send_binary(esp_xiaozhi_websocket_handle_t handle, const uint8_t *data, size_t data_len)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "Invalid handle");
    ESP_RETURN_ON_FALSE(data, ESP_ERR_INVALID_ARG, TAG, "Invalid data");
    ESP_RETURN_ON_FALSE(data_len > 0, ESP_ERR_INVALID_ARG, TAG, "Invalid data_len");

    esp_xiaozhi_websocket_t *websocket = (esp_xiaozhi_websocket_t *)handle;
    esp_err_t ret = esp_xiaozhi_websocket_lock(websocket);
    ESP_RETURN_ON_ERROR(ret, TAG, "Failed to lock websocket lifecycle");
    if (websocket->stopping || websocket->websocket_client == NULL) {
        xSemaphoreGive(websocket->lifecycle_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    if (!esp_xiaozhi_websocket_client_is_connected(websocket)) {
        if (websocket->websocket_client != NULL &&
                !esp_websocket_client_is_connected(websocket->websocket_client)) {
            esp_xiaozhi_websocket_notify_disconnected(websocket);
        }
        xSemaphoreGive(websocket->lifecycle_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    int sent = esp_websocket_client_send_bin(
        websocket->websocket_client, (const char *)data, data_len,
        pdMS_TO_TICKS(ESP_XIAOZHI_WEBSOCKET_BINARY_SEND_TIMEOUT_MS));
    if (sent < 0 || (size_t)sent != data_len) {
        ret = esp_xiaozhi_websocket_send_failed(
            websocket, sent, data_len, "binary");
    } else {
        ret = ESP_OK;
    }
    xSemaphoreGive(websocket->lifecycle_mutex);
    return ret;
}

esp_err_t esp_xiaozhi_websocket_set_callbacks(esp_xiaozhi_websocket_handle_t handle, esp_xiaozhi_transport_handle_t transport_handle, const esp_xiaozhi_transport_callbacks_t *callbacks)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "Invalid handle");
    ESP_RETURN_ON_FALSE(callbacks, ESP_ERR_INVALID_ARG, TAG, "Invalid callbacks");

    esp_xiaozhi_websocket_t *websocket = (esp_xiaozhi_websocket_t *)handle;
    websocket->transport_handle = transport_handle;
    websocket->callbacks = *callbacks;

    return ESP_OK;
}
