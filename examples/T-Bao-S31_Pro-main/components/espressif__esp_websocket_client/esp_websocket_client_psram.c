/*
 * Keep Espressif's WebSocket implementation, but keep its transport task
 * stack in internal RAM. The transport path can enter TLS/flash code while
 * the cache is disabled, and ESP-IDF rejects an external-RAM current stack in
 * that path. Receive payloads remain PSRAM-backed in the XiaoZhi wrapper; only
 * task stacks are deliberately kept in internal RAM. The TCB remains in
 * internal RAM, and the owner-side reclamation below still protects every
 * reconnect transition.
 */
#include <stdbool.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_websocket_client_psram.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

static const char *WS_ALLOC_TAG = "xiaozhi_ws_alloc";
#define ESP_WEBSOCKET_TASK_CREATE_MAX_ATTEMPTS 3U
#define ESP_WEBSOCKET_TASK_CREATE_RETRY_DELAY_MS 100U
#define ESP_WEBSOCKET_TASK_OWNER_ACQUIRE_ATTEMPTS 100U
#define ESP_WEBSOCKET_TASK_OWNER_RETRY_DELAY_MS 10U
#define ESP_WEBSOCKET_TASK_QUIESCENT_ATTEMPTS 500U
#define ESP_WEBSOCKET_TASK_RECLAIM_WAIT_ATTEMPTS 100U

/* WithCaps tasks must be deleted with vTaskDeleteWithCaps().  The upstream
 * client self-deletes at the end of its event loop, so intercept that one
 * call and leave the task suspended until the owner has stopped the client.
 * This keeps task/TCB ownership deterministic across reconnects. */
typedef struct {
    void *client;
    TaskHandle_t task;
    bool active;
    bool creation_in_progress;
    bool metadata_ready;
    bool reclaiming;
    bool with_caps;
    bool static_task;
    bool persistent_stack;
    StackType_t *stack;
} esp_websocket_task_state_t;

static esp_websocket_task_state_t s_websocket_task_state;
static portMUX_TYPE s_websocket_task_state_lock = portMUX_INITIALIZER_UNLOCKED;

#if configSUPPORT_STATIC_ALLOCATION == 1
/* There is one transport client in this firmware.  Keep the static TCB in
 * internal DRAM, but outside the create function so a stale owner transition
 * cannot accidentally create a second hidden TCB object. */
static DRAM_ATTR StaticTask_t s_websocket_task_tcb;
/* Reserve the static task stack once. Reusing this block across conversation
 * sessions avoids repeatedly fragmenting internal RAM during stop/start
 * cycles. */
static StackType_t *s_websocket_task_stack;
static bool s_websocket_task_stack_psram;
#endif

static void esp_websocket_task_wait_not_running(TaskHandle_t task);
static bool esp_websocket_task_reclaim_finished_state(void);
static bool esp_websocket_task_state_acquire(
    void *client, bool with_caps, TaskHandle_t *task_handle);
static void esp_websocket_task_delete_created(
    TaskHandle_t task, bool with_caps);
static void esp_websocket_task_state_clear_if_match(
    void *client, TaskHandle_t task);

static size_t esp_websocket_task_stack_bytes(
    configSTACK_DEPTH_TYPE stack_depth)
{
    return (size_t)stack_depth * sizeof(StackType_t);
}

static bool esp_websocket_task_stack_caps_are_internal(
    UBaseType_t memory_caps)
{
    if ((memory_caps & MALLOC_CAP_SPIRAM) != 0U ||
            (memory_caps & MALLOC_CAP_INTERNAL) == 0U) {
        ESP_LOGE(WS_ALLOC_TAG,
                 "Rejecting external/non-internal WebSocket task stack "
                 "caps=0x%08x",
                 (unsigned)memory_caps);
        return false;
    }
    return true;
}

/* STOPPED_BIT is set by the upstream task immediately before the delete
 * proxy suspends it.  The owner may observe that bit a few instructions
 * early, so wait briefly for a quiescent task state before touching its stack.
 * This also prevents a premature destroy path from deleting a live transport
 * task just because its stop event was already posted. */
static bool esp_websocket_task_wait_quiescent(
    TaskHandle_t task, eTaskState *state_out)
{
    if (task == NULL) {
        return false;
    }

    const TickType_t retry_delay = pdMS_TO_TICKS(1);
    for (unsigned attempt = 0;
            attempt < ESP_WEBSOCKET_TASK_QUIESCENT_ATTEMPTS;
            ++attempt) {
        const eTaskState state = eTaskGetState(task);
        if (state == eSuspended || state == eDeleted || state == eInvalid) {
            if (state_out != NULL) {
                *state_out = state;
            }
            return true;
        }
        if (retry_delay != 0) {
            vTaskDelay(retry_delay);
        } else {
            taskYIELD();
        }
    }

    if (state_out != NULL) {
        *state_out = eTaskGetState(task);
    }
    return false;
}

static void esp_websocket_task_delete_direct(TaskHandle_t task)
{
    vTaskDelete(task);
}

static bool esp_websocket_task_state_begin(
    void *client, bool with_caps, TaskHandle_t *task_handle)
{
    (void)task_handle;
    bool accepted = false;
    portENTER_CRITICAL(&s_websocket_task_state_lock);
    /* There is one XiaoZhi transport in this firmware, but keep the wrapper
     * defensive: an old suspended task must be reclaimed before another
     * client is allowed to overwrite the only owner record. */
    /* The owner record is a reservation, not just a task handle.  Keep it
     * occupied from the first create attempt until the matching task/stack
     * has been reclaimed; allowing a second create for the same client while
     * task == NULL would let two callers overwrite the single static TCB. */
    if (!s_websocket_task_state.active) {
        s_websocket_task_state.client = client;
        /* The upstream client handle is not reset by destroy().  Never carry
         * that stale value into a new owner record: the create wrapper (or the
         * task's delete proxy) publishes the current handle below. */
        s_websocket_task_state.task = NULL;
        s_websocket_task_state.active = true;
        s_websocket_task_state.creation_in_progress = true;
        s_websocket_task_state.metadata_ready = false;
        s_websocket_task_state.reclaiming = false;
        s_websocket_task_state.with_caps = with_caps;
        s_websocket_task_state.static_task = false;
        s_websocket_task_state.persistent_stack = false;
        s_websocket_task_state.stack = NULL;
        accepted = true;
    }
    portEXIT_CRITICAL(&s_websocket_task_state_lock);
    return accepted;
}

static void esp_websocket_task_state_clear_if_match(
    void *client, TaskHandle_t task)
{
    portENTER_CRITICAL(&s_websocket_task_state_lock);
    if (s_websocket_task_state.active &&
            s_websocket_task_state.client == client &&
            !s_websocket_task_state.reclaiming &&
            (task == NULL || s_websocket_task_state.task == task)) {
        memset(&s_websocket_task_state, 0, sizeof(s_websocket_task_state));
    }
    portEXIT_CRITICAL(&s_websocket_task_state_lock);
}

/* Return true when the single owner slot is free, or when a task that was
 * deliberately suspended by the delete proxy has been reclaimed.  A task that
 * is still Ready/Running/Blocked is never killed here: the current session's
 * owner must finish its normal destroy path first. */
static bool esp_websocket_task_reclaim_finished_state(void)
{
    esp_websocket_task_state_t snapshot;
    memset(&snapshot, 0, sizeof(snapshot));

    portENTER_CRITICAL(&s_websocket_task_state_lock);
    snapshot = s_websocket_task_state;
    portEXIT_CRITICAL(&s_websocket_task_state_lock);

    if (!snapshot.active) {
        return true;
    }
    if (snapshot.reclaiming) {
        return false;
    }
    if (!snapshot.metadata_ready) {
        /* The creator still owns the reservation while it is publishing the
         * allocator metadata. Reclaiming here could free the static TCB/stack
         * before xTaskCreate*() has returned to that creator. */
        if (snapshot.creation_in_progress) {
            return false;
        }
        if (snapshot.task == NULL) {
            /* No creator is still running and no task was published. This is
             * an abandoned reservation from an allocator failure; release it
             * before the next wake attempts to reuse the owner slot. */
            StackType_t *orphan_stack = snapshot.static_task &&
                !snapshot.persistent_stack ? snapshot.stack : NULL;
            bool cleared = false;
            portENTER_CRITICAL(&s_websocket_task_state_lock);
            if (s_websocket_task_state.active &&
                    s_websocket_task_state.client == snapshot.client &&
                    !s_websocket_task_state.reclaiming &&
                    !s_websocket_task_state.creation_in_progress &&
                    s_websocket_task_state.task == NULL &&
                    s_websocket_task_state.static_task == snapshot.static_task &&
                    s_websocket_task_state.stack == snapshot.stack) {
                memset(&s_websocket_task_state, 0,
                       sizeof(s_websocket_task_state));
                cleared = true;
            }
            portEXIT_CRITICAL(&s_websocket_task_state_lock);
            if (cleared) {
                if (orphan_stack != NULL) {
                    heap_caps_free(orphan_stack);
                }
                ESP_LOGW(WS_ALLOC_TAG,
                         "Reclaimed abandoned WebSocket task owner reservation");
                return true;
            }
        }
        /* The task may already have reached the upstream self-delete call,
         * but its allocator/stack metadata is still being published by the
         * creator.  Never reclaim with an incomplete allocator description. */
        return false;
    }
    if (snapshot.task == NULL) {
        /* A creator may have published the owner record just before the task
         * handle is returned.  Do not free anything from that transition. */
        return false;
    }

    bool claimed = false;
    portENTER_CRITICAL(&s_websocket_task_state_lock);
    if (s_websocket_task_state.active &&
            s_websocket_task_state.client == snapshot.client &&
            s_websocket_task_state.task == snapshot.task &&
            !s_websocket_task_state.reclaiming &&
            s_websocket_task_state.static_task == snapshot.static_task &&
            s_websocket_task_state.stack == snapshot.stack) {
        /* Keep the owner slot active until the old task/stack has actually
         * been deleted.  Otherwise a concurrent wake could reuse the global
         * static TCB while the old task is still present in the scheduler. */
        s_websocket_task_state.reclaiming = true;
        claimed = true;
    }
    portEXIT_CRITICAL(&s_websocket_task_state_lock);
    if (!claimed) {
        /* Another owner is already deleting the task.  Keep reporting the
         * slot as busy until that owner clears the state; returning true here
         * makes callers spin and obscures the real handoff in diagnostics. */
        return false;
    }

    eTaskState task_state = eInvalid;
    if (!esp_websocket_task_wait_quiescent(snapshot.task, &task_state)) {
        portENTER_CRITICAL(&s_websocket_task_state_lock);
        if (s_websocket_task_state.active &&
                s_websocket_task_state.client == snapshot.client &&
                s_websocket_task_state.task == snapshot.task &&
                s_websocket_task_state.reclaiming) {
            s_websocket_task_state.reclaiming = false;
        }
        portEXIT_CRITICAL(&s_websocket_task_state_lock);
        return false;
    }
    const bool task_gone = task_state == eDeleted || task_state == eInvalid;
    if (!task_gone && task_state != eSuspended) {
        portENTER_CRITICAL(&s_websocket_task_state_lock);
        if (s_websocket_task_state.active &&
                s_websocket_task_state.client == snapshot.client &&
                s_websocket_task_state.task == snapshot.task &&
                s_websocket_task_state.reclaiming) {
            s_websocket_task_state.reclaiming = false;
        }
        portEXIT_CRITICAL(&s_websocket_task_state_lock);
        return false;
    }

    if (!task_gone) {
        /* Close the final resume window before deleting or freeing a static
         * stack. The state check above is only a snapshot. */
        vTaskSuspend(snapshot.task);
        if (snapshot.static_task) {
            esp_websocket_task_wait_not_running(snapshot.task);
            vTaskDelete(snapshot.task);
        }
#if configSUPPORT_STATIC_ALLOCATION == 1
        else if (snapshot.with_caps) {
            vTaskDeleteWithCaps(snapshot.task);
        }
#else
        else {
            vTaskDelete(snapshot.task);
        }
#endif
    } else if (snapshot.static_task) {
        /* eDeleted/eInvalid may be observed during a cross-core transition;
         * do not return the external stack until it is no longer current. */
        esp_websocket_task_wait_not_running(snapshot.task);
    }
    if (snapshot.static_task && !snapshot.persistent_stack &&
            snapshot.stack != NULL) {
        heap_caps_free(snapshot.stack);
    }
    portENTER_CRITICAL(&s_websocket_task_state_lock);
    if (s_websocket_task_state.active &&
            s_websocket_task_state.client == snapshot.client &&
            s_websocket_task_state.task == snapshot.task &&
            s_websocket_task_state.reclaiming &&
            s_websocket_task_state.static_task == snapshot.static_task &&
            s_websocket_task_state.stack == snapshot.stack) {
        memset(&s_websocket_task_state, 0, sizeof(s_websocket_task_state));
    }
    portEXIT_CRITICAL(&s_websocket_task_state_lock);
    ESP_LOGW(WS_ALLOC_TAG,
             "Reclaimed stale WebSocket task owner state (task=%p, stack=%s)",
             (void *)snapshot.task,
             snapshot.static_task ? "static" :
                 (snapshot.with_caps ? "caps" : "internal"));
    return true;
}

static bool esp_websocket_task_state_acquire(
    void *client, bool with_caps, TaskHandle_t *task_handle)
{
    /* Creation can race the final proxy suspend from the previous session.
     * Give that owner a few scheduler ticks to publish STOPPED and reclaim its
     * stack before declaring the new wake out of memory. */
    for (unsigned attempt = 0;
            attempt < ESP_WEBSOCKET_TASK_OWNER_ACQUIRE_ATTEMPTS;
            ++attempt) {
        if (esp_websocket_task_state_begin(client, with_caps, task_handle)) {
            return true;
        }
        if (esp_websocket_task_reclaim_finished_state()) {
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(ESP_WEBSOCKET_TASK_OWNER_RETRY_DELAY_MS));
    }
    return false;
}

static void esp_websocket_task_state_fail(void *client)
{
    StackType_t *orphan_stack = NULL;
    bool cleared = false;
    bool task_was_published = false;
    portENTER_CRITICAL(&s_websocket_task_state_lock);
    if (s_websocket_task_state.active &&
            s_websocket_task_state.client == client &&
            !s_websocket_task_state.reclaiming &&
            s_websocket_task_state.task == NULL) {
        if (s_websocket_task_state.static_task &&
                !s_websocket_task_state.persistent_stack) {
            orphan_stack = s_websocket_task_state.stack;
        }
        memset(&s_websocket_task_state, 0, sizeof(s_websocket_task_state));
        cleared = true;
    } else if (s_websocket_task_state.active &&
            s_websocket_task_state.client == client &&
            !s_websocket_task_state.reclaiming) {
        /* A task can reach the delete proxy before the creator receives the
         * return value. The handle and allocator mode are enough to make this
         * owner reclaimable; mark the publication complete instead of leaving
         * a permanent active reservation after a failed create call. */
        s_websocket_task_state.creation_in_progress = false;
        s_websocket_task_state.metadata_ready =
            s_websocket_task_state.task != NULL;
        task_was_published = s_websocket_task_state.metadata_ready;
    }
    portEXIT_CRITICAL(&s_websocket_task_state_lock);
    if (orphan_stack != NULL) {
        heap_caps_free(orphan_stack);
    }
    if (task_was_published) {
        ESP_LOGW(WS_ALLOC_TAG,
                 "WebSocket task create failed after task publication; "
                 "deferring matching task reclaim");
    }
    if (!cleared && !task_was_published) {
        ESP_LOGW(WS_ALLOC_TAG,
                 "WebSocket task create failure could not clear owner state; "
                 "task is still owned by another lifecycle path");
    }
}

/* This function is used only while compiling the upstream source below. */
static void esp_websocket_task_delete_proxy(TaskHandle_t task)
{
    const TaskHandle_t current = xTaskGetCurrentTaskHandle();
    if (task == NULL) {
        task = current;
    }

    bool tracked = false;
    portENTER_CRITICAL(&s_websocket_task_state_lock);
    /* Once the owner has claimed reclamation, the transport task can still
     * reach this final self-delete call on the other core.  Keep intercepting
     * the matching task while reclaiming; letting it fall through to the real
     * vTaskDelete() would race the owner deleting/freeing the same TCB/stack.
     * An unrelated task is never intercepted, even if the owner slot is in
     * the reclaiming state. */
    if (s_websocket_task_state.active && task == current &&
            (s_websocket_task_state.task == NULL ||
             s_websocket_task_state.task == task)) {
        s_websocket_task_state.task = task;
        tracked = true;
    }
    portEXIT_CRITICAL(&s_websocket_task_state_lock);

    if (!tracked) {
        esp_websocket_task_delete_direct(task);
        return;
    }

    /* The upstream task sets STOPPED_BIT immediately before this call. The
     * owner waits for that bit, then calls the public reclaim hook below. */
    vTaskSuspend(NULL);
}

static BaseType_t esp_websocket_task_create_with_caps(
    TaskFunction_t task_entry,
    const char *task_name,
    configSTACK_DEPTH_TYPE stack_depth,
    void *task_arg,
    UBaseType_t priority,
    TaskHandle_t *task_handle,
    BaseType_t core_id,
    UBaseType_t memory_caps)
{
    if (task_handle == NULL ||
            !esp_websocket_task_stack_caps_are_internal(memory_caps)) {
        return pdFAIL;
    }
#if configSUPPORT_STATIC_ALLOCATION == 1
    return xTaskCreatePinnedToCoreWithCaps(
        task_entry, task_name, stack_depth, task_arg, priority, task_handle,
        core_id, memory_caps);
#else
    (void)memory_caps;
    return xTaskCreatePinnedToCore(
        task_entry, task_name, stack_depth, task_arg, priority, task_handle,
        core_id);
#endif
}

static BaseType_t esp_websocket_task_create_static(
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
    /* The upstream start path always supplies the client task handle.  Do not
     * accept a NULL output here: a statically-created task would otherwise be
     * live but invisible to the owner/reclaimer, and the next wake could free
     * its stack while it is still running. */
    if (task_handle == NULL) {
        return pdFAIL;
    }
    if (!esp_websocket_task_stack_caps_are_internal(memory_caps)) {
        return pdFAIL;
    }

    /* FreeRTOS requires the TCB in internal RAM. Keep this explicit so a
     * future external-BSS configuration cannot move it out of DRAM. */
    /* FreeRTOS expresses stack depth in StackType_t entries. On ESP32-S31
     * StackType_t is one byte, but multiplying here keeps the allocator
     * correct if the component is reused on a port with wider stack entries. */
    const size_t stack_bytes = esp_websocket_task_stack_bytes(stack_depth);
    const bool wants_psram = (memory_caps & MALLOC_CAP_SPIRAM) != 0;
    StackType_t *stack = s_websocket_task_stack;

    /* The reusable reserve can outlive a failed create. Keep its capability
     * metadata attached to the allocation so a stale external block can never
     * be reused for this cache-sensitive task. */
    if (stack != NULL && s_websocket_task_stack_psram != wants_psram) {
        const bool previous_stack_psram = s_websocket_task_stack_psram;
        bool can_replace = false;
        portENTER_CRITICAL(&s_websocket_task_state_lock);
        if (s_websocket_task_state.active &&
                s_websocket_task_state.client == task_arg &&
                s_websocket_task_state.creation_in_progress &&
                !s_websocket_task_state.reclaiming &&
                s_websocket_task_state.task == NULL &&
                s_websocket_task_state.stack == NULL) {
            s_websocket_task_stack = NULL;
            s_websocket_task_stack_psram = false;
            can_replace = true;
        }
        portEXIT_CRITICAL(&s_websocket_task_state_lock);
        if (!can_replace) {
            ESP_LOGE(WS_ALLOC_TAG,
                     "Cannot replace WebSocket task stack reserve while "
                     "another owner is active (stack=%p)",
                     (void *)stack);
            return pdFAIL;
        }
        ESP_LOGW(WS_ALLOC_TAG,
                 "Replacing WebSocket task stack reserve (%s -> %s)",
                 previous_stack_psram ? "PSRAM" : "internal RAM",
                 wants_psram ? "PSRAM" : "internal RAM");
        heap_caps_free(stack);
        stack = NULL;
    }

    if (stack == NULL) {
        stack = (StackType_t *)heap_caps_malloc(stack_bytes, memory_caps);
        if (stack == NULL) {
            return pdFAIL;
        }
        s_websocket_task_stack = stack;
        s_websocket_task_stack_psram = wants_psram;
        ESP_LOGI(WS_ALLOC_TAG,
                 "Reserved reusable WebSocket task stack in %s (%u bytes)",
                 s_websocket_task_stack_psram ? "PSRAM" : "internal RAM",
                 (unsigned)stack_bytes);
    } else {
        ESP_LOGD(WS_ALLOC_TAG,
                 "Reusing WebSocket task stack in %s (%u bytes)",
                 s_websocket_task_stack_psram ? "PSRAM" : "internal RAM",
                 (unsigned)stack_bytes);
    }

    /* Publish the stack ownership before the task can run. If the static
     * create call fails, the owner record remains associated with the reusable
     * block; if the task runs immediately, the delete proxy cannot race an
     * unknown stack. */
    bool stack_reserved = false;
    portENTER_CRITICAL(&s_websocket_task_state_lock);
    if (s_websocket_task_state.active &&
            s_websocket_task_state.client == task_arg &&
            s_websocket_task_state.creation_in_progress &&
            !s_websocket_task_state.reclaiming) {
        s_websocket_task_state.static_task = true;
        s_websocket_task_state.persistent_stack = true;
        s_websocket_task_state.stack = stack;
        s_websocket_task_state.with_caps = false;
        stack_reserved = true;
    }
    portEXIT_CRITICAL(&s_websocket_task_state_lock);
    if (!stack_reserved) {
        /* Keep the reserved stack for a later owner retry.  Freeing it here
         * would reintroduce the PSRAM fragmentation this wrapper is meant to
         * prevent. */
        return pdFAIL;
    }

    TaskHandle_t task = xTaskCreateStaticPinnedToCore(
        task_entry, task_name, (uint32_t)stack_depth, task_arg, priority,
        stack, &s_websocket_task_tcb, core_id);
    if (task == NULL) {
        /* Keep the pre-published pointer for state_fail(), which owns the
         * reservation cleanup on every failed allocator attempt. */
        return pdFAIL;
    }

    bool tracked = false;
    portENTER_CRITICAL(&s_websocket_task_state_lock);
    if (s_websocket_task_state.active &&
            s_websocket_task_state.client == task_arg &&
            s_websocket_task_state.creation_in_progress &&
            !s_websocket_task_state.reclaiming) {
        s_websocket_task_state.static_task = true;
        s_websocket_task_state.stack = stack;
        s_websocket_task_state.persistent_stack = true;
        s_websocket_task_state.task = task;
        s_websocket_task_state.with_caps = false;
        s_websocket_task_state.metadata_ready = true;
        s_websocket_task_state.creation_in_progress = false;
        tracked = true;
    }
    portEXIT_CRITICAL(&s_websocket_task_state_lock);

    /* The owner record must be published before the task can be reclaimed.
     * If a concurrent teardown invalidated it while the task was being
     * created, delete the just-created static task and release its stack
     * instead of leaving an untracked task behind. */
    if (!tracked) {
        vTaskSuspend(task);
        esp_websocket_task_wait_not_running(task);
        vTaskDelete(task);
        esp_websocket_task_state_clear_if_match(task_arg, task);
        return pdFAIL;
    }

    *task_handle = task;
    return pdPASS;
#else
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

static void esp_websocket_task_wait_not_running(TaskHandle_t task)
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
        /* Let the idle task and the suspended task's owning core make
         * progress; a tight yield loop can trip the task watchdog while a
         * cross-core delete is settling. */
        vTaskDelay(1);
    }
}

/* Dispose of a task that was created successfully but could not be published
 * into the owner record. This is a defensive path for an unexpected lifecycle
 * race; leaving such a task alive would make the next wake look like an
 * allocation failure and could free a live external stack. */
static void esp_websocket_task_delete_created(
    TaskHandle_t task, bool with_caps)
{
    if (task == NULL) {
        return;
    }

    const eTaskState state = eTaskGetState(task);
    if (state == eDeleted || state == eInvalid) {
        return;
    }

    vTaskSuspend(task);
    esp_websocket_task_wait_not_running(task);
#if configSUPPORT_STATIC_ALLOCATION == 1
    if (with_caps) {
        vTaskDeleteWithCaps(task);
        return;
    }
#else
    (void)with_caps;
#endif
    vTaskDelete(task);
}

static BaseType_t esp_websocket_task_create_with_fallback(
    TaskFunction_t task_entry,
    const char *task_name,
    const configSTACK_DEPTH_TYPE stack_depth,
    void *task_arg,
    UBaseType_t priority,
    TaskHandle_t *task_handle,
    BaseType_t core_id)
{
    /* Emit this before attempting the allocator. If this marker is absent from
     * device logs, the local component override is not in the flashed image;
     * the success-only stack message cannot distinguish that case from an
     * allocation failure. */
    ESP_LOGI(WS_ALLOC_TAG,
             "WebSocket task allocator active (internal RAM only; PSRAM reserved for payloads; stack=%u bytes)",
             (unsigned)esp_websocket_task_stack_bytes(stack_depth));

    /* A successful task create must always publish a handle.  The upstream
     * client passes &client->task_handle; rejecting a NULL pointer here keeps
     * the wrapper safe if a future caller violates that contract. */
    if (task_handle == NULL) {
        ESP_LOGE(WS_ALLOC_TAG, "WebSocket task create requires a task handle");
        return pdFAIL;
    }
    *task_handle = NULL;

    for (unsigned attempt = 1; attempt <= ESP_WEBSOCKET_TASK_CREATE_MAX_ATTEMPTS;
            ++attempt) {
        *task_handle = NULL;
        BaseType_t ret = pdFAIL;
#if CONFIG_SPIRAM && (configSUPPORT_STATIC_ALLOCATION == 1)
        /* Keep the transport stack in internal RAM because TLS/flash paths can
         * disable the cache while this task is current. Prefer a caller-owned
         * static TCB so this path does not consume another internal heap block. */
        if (!esp_websocket_task_state_acquire(task_arg, false, task_handle)) {
            ESP_LOGE(WS_ALLOC_TAG,
                     "WebSocket task allocation blocked by an active previous client");
            return pdFAIL;
        }
        ret = esp_websocket_task_create_static(
            task_entry, task_name, stack_depth, task_arg, priority,
            task_handle, core_id, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (ret == pdPASS && *task_handle != NULL) {
            ESP_LOGI(WS_ALLOC_TAG,
                     "WebSocket task stack allocated in internal RAM (%u bytes)",
                     (unsigned)esp_websocket_task_stack_bytes(stack_depth));
            ESP_LOGI(WS_ALLOC_TAG, "WebSocket task uses a static internal TCB");
            return ret;
        }
        if (ret == pdPASS) {
            ESP_LOGE(WS_ALLOC_TAG,
                     "WebSocket task allocator returned success without a handle");
        }
        esp_websocket_task_state_fail(task_arg);

        if (!esp_websocket_task_state_acquire(task_arg, true, task_handle)) {
            ESP_LOGE(WS_ALLOC_TAG,
                     "WebSocket task allocation blocked by an active previous client");
            return pdFAIL;
        }
        ret = esp_websocket_task_create_with_caps(
            task_entry, task_name, stack_depth, task_arg, priority,
            task_handle, core_id, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#endif
        if (ret == pdPASS && *task_handle != NULL) {
            bool published = false;
            portENTER_CRITICAL(&s_websocket_task_state_lock);
            if (s_websocket_task_state.active &&
                    s_websocket_task_state.client == task_arg &&
                    s_websocket_task_state.creation_in_progress &&
                    (s_websocket_task_state.task == NULL ||
                     s_websocket_task_state.task == *task_handle)) {
                s_websocket_task_state.task = *task_handle;
                s_websocket_task_state.metadata_ready = true;
                s_websocket_task_state.creation_in_progress = false;
                published = true;
            }
            portEXIT_CRITICAL(&s_websocket_task_state_lock);
            if (published) {
                ESP_LOGI(WS_ALLOC_TAG, "WebSocket task stack allocated in internal RAM (%u bytes)",
                         (unsigned)esp_websocket_task_stack_bytes(stack_depth));
                return ret;
            }
            ESP_LOGE(WS_ALLOC_TAG,
                     "WebSocket task created but owner publication was lost; deleting it");
            esp_websocket_task_delete_created(*task_handle, true);
            esp_websocket_task_state_clear_if_match(task_arg, *task_handle);
            *task_handle = NULL;
        }
        if (ret == pdPASS) {
            ESP_LOGE(WS_ALLOC_TAG,
                     "WebSocket task allocator returned success without a handle");
        }
        esp_websocket_task_state_fail(task_arg);

        const size_t psram_free =
            heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        const size_t psram_largest =
            heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        const size_t internal_free =
            heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        const size_t internal_largest =
            heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        ESP_LOGW(WS_ALLOC_TAG,
                 "WebSocket task allocation failed (attempt=%u/%u, stack=%u; "
                 "PSRAM free=%u largest=%u; internal free=%u largest=%u)",
                 attempt, ESP_WEBSOCKET_TASK_CREATE_MAX_ATTEMPTS,
                     (unsigned)esp_websocket_task_stack_bytes(stack_depth), (unsigned)psram_free,
                 (unsigned)psram_largest, (unsigned)internal_free,
                 (unsigned)internal_largest);

#if configSUPPORT_STATIC_ALLOCATION == 1
        /* Keep the fallback on the same allocator/reclaimer pair. Prefer a
         * static TCB here as well; all WebSocket task stacks remain internal so
         * cache-disabled TLS/flash paths are always safe. */
        if (!esp_websocket_task_state_acquire(task_arg, false, task_handle)) {
            ESP_LOGE(WS_ALLOC_TAG,
                     "WebSocket internal fallback blocked by an active previous client");
            return pdFAIL;
        }
        ret = esp_websocket_task_create_static(
            task_entry, task_name, stack_depth, task_arg, priority,
            task_handle, core_id, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (ret == pdPASS && *task_handle != NULL) {
            ESP_LOGI(WS_ALLOC_TAG,
                     "WebSocket task stack allocated in internal RAM (%u bytes)",
                     (unsigned)esp_websocket_task_stack_bytes(stack_depth));
            ESP_LOGI(WS_ALLOC_TAG, "WebSocket task uses a static internal TCB");
            return ret;
        }
        if (ret == pdPASS) {
            ESP_LOGE(WS_ALLOC_TAG,
                     "WebSocket task allocator returned success without a handle");
        }
        esp_websocket_task_state_fail(task_arg);

        if (!esp_websocket_task_state_acquire(task_arg, true, task_handle)) {
            ESP_LOGE(WS_ALLOC_TAG,
                     "WebSocket internal fallback blocked by an active previous client");
            return pdFAIL;
        }
        ret = esp_websocket_task_create_with_caps(
            task_entry, task_name, stack_depth, task_arg, priority, task_handle,
            core_id, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
        if (!esp_websocket_task_state_acquire(task_arg, false, task_handle)) {
            ESP_LOGE(WS_ALLOC_TAG,
                     "WebSocket task fallback blocked by an active previous client");
            return pdFAIL;
        }
        ret = xTaskCreatePinnedToCore(
            task_entry, task_name, stack_depth, task_arg, priority, task_handle,
            core_id);
#endif
        if (ret == pdPASS && *task_handle != NULL) {
            bool published = false;
            portENTER_CRITICAL(&s_websocket_task_state_lock);
            if (s_websocket_task_state.active &&
                    s_websocket_task_state.client == task_arg &&
                    s_websocket_task_state.creation_in_progress &&
                    (s_websocket_task_state.task == NULL ||
                     s_websocket_task_state.task == *task_handle)) {
                s_websocket_task_state.task = *task_handle;
                s_websocket_task_state.metadata_ready = true;
                s_websocket_task_state.creation_in_progress = false;
                published = true;
            }
            portEXIT_CRITICAL(&s_websocket_task_state_lock);
            if (published) {
                ESP_LOGI(WS_ALLOC_TAG, "WebSocket task stack allocated in internal RAM (%u bytes)",
                         (unsigned)esp_websocket_task_stack_bytes(stack_depth));
                return ret;
            }
            ESP_LOGE(WS_ALLOC_TAG,
                     "WebSocket task created but owner publication was lost; deleting it");
            esp_websocket_task_delete_created(*task_handle, true);
            esp_websocket_task_state_clear_if_match(task_arg, *task_handle);
            *task_handle = NULL;
        }
        esp_websocket_task_state_fail(task_arg);

        ESP_LOGW(WS_ALLOC_TAG,
                 "Internal WebSocket task allocation failed (attempt=%u/%u; "
                 "PSRAM free=%u largest=%u; internal free=%u largest=%u)",
                 attempt, ESP_WEBSOCKET_TASK_CREATE_MAX_ATTEMPTS,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

        if (attempt < ESP_WEBSOCKET_TASK_CREATE_MAX_ATTEMPTS) {
            /* Let the idle task reclaim any recently deleted task resources
             * before retrying; retries are bounded to avoid blocking wakeup. */
            vTaskDelay(pdMS_TO_TICKS(ESP_WEBSOCKET_TASK_CREATE_RETRY_DELAY_MS));
        }
    }

    ESP_LOGE(WS_ALLOC_TAG,
             "WebSocket task allocation exhausted after %u attempts "
             "(stack=%u; PSRAM free=%u largest=%u; internal free=%u largest=%u)",
              ESP_WEBSOCKET_TASK_CREATE_MAX_ATTEMPTS,
              (unsigned)esp_websocket_task_stack_bytes(stack_depth),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    if (task_handle != NULL) {
        *task_handle = NULL;
    }
    return pdFAIL;
}

esp_err_t esp_websocket_client_psram_reclaim_task(
    esp_websocket_client_handle_t client)
{
    if (client == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const TaskHandle_t current = xTaskGetCurrentTaskHandle();
    unsigned owner_wait_attempts = 0;
    esp_websocket_task_state_t snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    bool claimed = false;

reclaim_retry:
    portENTER_CRITICAL(&s_websocket_task_state_lock);
    if (!s_websocket_task_state.active) {
        portEXIT_CRITICAL(&s_websocket_task_state_lock);
        return ESP_OK;
    }
    if (s_websocket_task_state.client != (void *)client) {
        portEXIT_CRITICAL(&s_websocket_task_state_lock);
        /* A previous client may already have reached the proxy's suspended
         * handoff while its cleanup owner is between stop() and destroy().
         * Reclaim that finished owner before rejecting the new client; this
         * closes the reset/reconnect window without ever deleting a task that
         * is still Ready/Running/Blocked. */
        if (esp_websocket_task_reclaim_finished_state()) {
            return ESP_OK;
        }
        if (++owner_wait_attempts < ESP_WEBSOCKET_TASK_RECLAIM_WAIT_ATTEMPTS) {
            vTaskDelay(pdMS_TO_TICKS(ESP_WEBSOCKET_TASK_OWNER_RETRY_DELAY_MS));
            goto reclaim_retry;
        }
        ESP_LOGW(WS_ALLOC_TAG,
                 "Refusing to reclaim a WebSocket task owned by another active client");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_websocket_task_state.reclaiming) {
        const TaskHandle_t owner_task = s_websocket_task_state.task;
        portEXIT_CRITICAL(&s_websocket_task_state_lock);
        if (owner_task == current) {
            ESP_LOGE(WS_ALLOC_TAG,
                     "Refusing to reclaim WebSocket task from its own context");
            return ESP_ERR_INVALID_STATE;
        }
        if (++owner_wait_attempts < ESP_WEBSOCKET_TASK_RECLAIM_WAIT_ATTEMPTS) {
            vTaskDelay(pdMS_TO_TICKS(ESP_WEBSOCKET_TASK_OWNER_RETRY_DELAY_MS));
            goto reclaim_retry;
        }
        ESP_LOGW(WS_ALLOC_TAG,
                 "Timed out waiting for another WebSocket task reclaim owner");
        return ESP_ERR_TIMEOUT;
    }

    snapshot = s_websocket_task_state;
    if (!snapshot.metadata_ready) {
        if (snapshot.creation_in_progress) {
            /* The create caller still owns the reservation and may be about
             * to publish a task handle. Do not clear the record or free a
             * pre-published static stack from a concurrent cleanup path. */
            portEXIT_CRITICAL(&s_websocket_task_state_lock);
            if (++owner_wait_attempts < ESP_WEBSOCKET_TASK_RECLAIM_WAIT_ATTEMPTS) {
                vTaskDelay(pdMS_TO_TICKS(ESP_WEBSOCKET_TASK_OWNER_RETRY_DELAY_MS));
                goto reclaim_retry;
            }
            ESP_LOGW(WS_ALLOC_TAG,
                     "Timed out waiting for WebSocket task creation metadata");
            return ESP_ERR_TIMEOUT;
        }
        /* A failed xTaskCreate*() can leave the reservation visible for a
         * short period even though no task handle was ever published.  The
         * caller is already past esp_websocket_client_start(), so there is no
         * creator left that could still publish a task. Clear that empty
         * reservation here; otherwise every later wake is rejected as a stale
         * owner/NO_MEM condition. Never clear a non-NULL task: that would
         * orphan a live stack. */
        if (snapshot.task == NULL) {
            StackType_t *orphan_stack = snapshot.static_task &&
                !snapshot.persistent_stack ? snapshot.stack : NULL;
            bool cleared = false;
            if (s_websocket_task_state.active &&
                    s_websocket_task_state.client == snapshot.client &&
                    !s_websocket_task_state.reclaiming &&
                    !s_websocket_task_state.creation_in_progress &&
                    s_websocket_task_state.task == NULL &&
                    s_websocket_task_state.static_task == snapshot.static_task &&
                    s_websocket_task_state.stack == snapshot.stack) {
                memset(&s_websocket_task_state, 0,
                       sizeof(s_websocket_task_state));
                cleared = true;
            }
            portEXIT_CRITICAL(&s_websocket_task_state_lock);
            if (cleared && orphan_stack != NULL) {
                heap_caps_free(orphan_stack);
                ESP_LOGW(WS_ALLOC_TAG,
                         "Reclaimed untracked WebSocket task stack after create failure");
            }
            return cleared ? ESP_OK : ESP_ERR_INVALID_STATE;
        }
        portEXIT_CRITICAL(&s_websocket_task_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (snapshot.task == current) {
        portEXIT_CRITICAL(&s_websocket_task_state_lock);
        ESP_LOGE(WS_ALLOC_TAG,
                 "Refusing to reclaim WebSocket task from its own context");
        return ESP_ERR_INVALID_STATE;
    }
    /* Claim before touching the scheduler.  The stale-owner helper uses the
     * same flag, so a retry or a concurrent destroy cannot delete this task or
     * free its stack a second time. */
    s_websocket_task_state.reclaiming = true;
    claimed = true;
    portEXIT_CRITICAL(&s_websocket_task_state_lock);

    if (!claimed) {
        return ESP_ERR_INVALID_STATE;
    }

    if (snapshot.task == NULL) {
        /* A create attempt can fail after publishing the owner record, and a
         * destroy path can consequently reach reclaim without a task handle.
         * Drop that record so the next wake is not permanently blocked by an
         * active-but-empty owner slot.  A static-stack attempt can also leave
         * an allocated stack behind; detach it under the same lock and free it
         * after leaving the critical section. */
        StackType_t *orphan_stack = snapshot.static_task &&
            !snapshot.persistent_stack ? snapshot.stack : NULL;
        portENTER_CRITICAL(&s_websocket_task_state_lock);
        if (s_websocket_task_state.active &&
                s_websocket_task_state.client == (void *)client &&
                s_websocket_task_state.reclaiming) {
            memset(&s_websocket_task_state, 0, sizeof(s_websocket_task_state));
        }
        portEXIT_CRITICAL(&s_websocket_task_state_lock);
        if (orphan_stack != NULL) {
            heap_caps_free(orphan_stack);
            ESP_LOGW(WS_ALLOC_TAG, "Reclaimed orphaned WebSocket task stack");
        }
        ESP_LOGW(WS_ALLOC_TAG,
                 "WebSocket task reclaim had no task handle; owner state cleared");
        return ESP_OK;
    }

    /* STOPPED_BIT is set immediately before the upstream self-delete call.
     * Suspend the tracked task explicitly so the allocator-specific delete
     * below cannot race the final instructions on the other core. */
    eTaskState task_state = eInvalid;
    if (!esp_websocket_task_wait_quiescent(snapshot.task, &task_state)) {
        portENTER_CRITICAL(&s_websocket_task_state_lock);
        if (s_websocket_task_state.active &&
                s_websocket_task_state.client == (void *)client &&
                s_websocket_task_state.task == snapshot.task &&
                s_websocket_task_state.reclaiming) {
            s_websocket_task_state.reclaiming = false;
        }
        portEXIT_CRITICAL(&s_websocket_task_state_lock);
        ESP_LOGW(WS_ALLOC_TAG,
                 "WebSocket task did not become quiescent before reclaim");
        return ESP_ERR_TIMEOUT;
    }
    const bool task_gone = task_state == eDeleted || task_state == eInvalid;
    if (!task_gone) {
        vTaskSuspend(snapshot.task);
    }
    if (snapshot.static_task) {
        /* A statically-created task is not eligible for vTaskDeleteWithCaps;
         * wait until it is no longer current before releasing its stack. */
        if (!task_gone) {
            esp_websocket_task_wait_not_running(snapshot.task);
            vTaskDelete(snapshot.task);
        } else {
            /* eDeleted/eInvalid can be observed while the scheduler is still
             * finishing a cross-core transition. Never return the external
             * stack to the heap until the handle is no longer current. */
            esp_websocket_task_wait_not_running(snapshot.task);
        }
    } else if (snapshot.with_caps && !task_gone) {
#if configSUPPORT_STATIC_ALLOCATION == 1
        vTaskDeleteWithCaps(snapshot.task);
#else
        vTaskDelete(snapshot.task);
#endif
    } else if (!task_gone) {
        vTaskDelete(snapshot.task);
    }
    if (snapshot.static_task && !snapshot.persistent_stack &&
            snapshot.stack != NULL) {
        heap_caps_free(snapshot.stack);
    }

    portENTER_CRITICAL(&s_websocket_task_state_lock);
    if (s_websocket_task_state.active &&
            s_websocket_task_state.client == (void *)client &&
            s_websocket_task_state.task == snapshot.task &&
            s_websocket_task_state.reclaiming &&
            s_websocket_task_state.static_task == snapshot.static_task &&
            s_websocket_task_state.stack == snapshot.stack) {
        memset(&s_websocket_task_state, 0, sizeof(s_websocket_task_state));
    }
    portEXIT_CRITICAL(&s_websocket_task_state_lock);
    ESP_LOGI(WS_ALLOC_TAG,
             "WebSocket task stack/TCB reclaimed (task=%p, stack=%s%s)",
             (void *)snapshot.task,
             snapshot.static_task ? "static" :
                 (snapshot.with_caps ? "caps" : "internal"),
             task_gone ? ", already stopped" : "");
    return ESP_OK;
}

esp_err_t esp_websocket_client_psram_release_task_storage(void)
{
#if configSUPPORT_STATIC_ALLOCATION == 1
    StackType_t *stack = NULL;
    portENTER_CRITICAL(&s_websocket_task_state_lock);
    if (s_websocket_task_state.active) {
        portEXIT_CRITICAL(&s_websocket_task_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    stack = s_websocket_task_stack;
    s_websocket_task_stack = NULL;
    s_websocket_task_stack_psram = false;
    portEXIT_CRITICAL(&s_websocket_task_state_lock);
    if (stack != NULL) {
        heap_caps_free(stack);
        ESP_LOGI(WS_ALLOC_TAG, "Released reusable WebSocket task stack");
    }
#else
    /* No static stack reserve exists when the IDF static-allocation additions
     * are disabled. */
#endif
    return ESP_OK;
}

/* The pinned upstream implementation has one task creation and one self-delete.
 * Route creation through the PSRAM-aware allocator and defer self-deletion to
 * the owner so WithCaps task memory is reclaimed without a helper allocation. */
#define xTaskCreatePinnedToCore esp_websocket_task_create_with_fallback
#define vTaskDelete esp_websocket_task_delete_proxy
#include "upstream/esp_websocket_client.c"
#undef vTaskDelete
#undef xTaskCreatePinnedToCore
