/*
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_websocket_client.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Reclaim the task created by the local lifecycle-safe WebSocket wrapper.
 *
 * The upstream client sets its stopped bit and then self-deletes. The wrapper
 * suspends that task instead so the owner can use the matching delete API
 * before destroying the opaque client object. Task stacks are kept in
 * internal RAM; PSRAM is reserved for payload buffers.
 */
esp_err_t esp_websocket_client_psram_reclaim_task(
    esp_websocket_client_handle_t client);

/**
 * Release the reusable static task stack after the final client is gone.
 * Returns ESP_ERR_INVALID_STATE while an owner/task is still active.
 */
esp_err_t esp_websocket_client_psram_release_task_storage(void);

#ifdef __cplusplus
}
#endif
