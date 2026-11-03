/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <esp_log.h>
#include <esp_check.h>
#include <esp_heap_caps.h>
#include <cJSON.h>
#include "esp_mcp_mgr.h"
#include "esp_xiaozhi_transport.h"
#include "esp_xiaozhi_mcp.h"

/**
 * @brief MCP transport item structure; uses esp_xiaozhi_transport for MCP.
 */
typedef struct esp_xiaozhi_mcp_item_s {
    esp_xiaozhi_mcp_config_t *config;       /*!< XiaoZhi MCP transport configuration */
    esp_xiaozhi_transport_handle_t transport_handle; /*!< Transport handle (MQTT or WebSocket) */
    esp_mcp_mgr_handle_t handle;            /*!< MCP manager handle */
} esp_xiaozhi_mcp_item_t;

static const char *TAG = "ESP_XIAOZHI_MCP";

static esp_xiaozhi_mcp_item_t *s_xiaozhi_mcp = NULL;

/* esp_mcp-c-sdk 2.0.1 stores request/session keys in 32-byte buffers. Keep
 * the full XiaoZhi id for the wire envelope, but use one deterministic key for
 * both session reset and SDK request context. */
#define ESP_XIAOZHI_MCP_ENGINE_SESSION_BUFFER_SIZE 32U
#define ESP_XIAOZHI_MCP_FALLBACK_SESSION_ID "xiaozhi-default"

static const char *esp_xiaozhi_mcp_make_engine_session_id(
    const char *transport_session_id,
    char *engine_session_id,
    size_t engine_session_id_size)
{
    if (!engine_session_id || engine_session_id_size < 2U) {
        return NULL;
    }

    if (!transport_session_id || transport_session_id[0] == '\0') {
        const int written = snprintf(engine_session_id, engine_session_id_size,
                                     "%s", ESP_XIAOZHI_MCP_FALLBACK_SESSION_ID);
        return (written >= 0 && (size_t)written < engine_session_id_size)
                   ? engine_session_id
                   : NULL;
    }

    const size_t session_id_len = strlen(transport_session_id);
    if (session_id_len < engine_session_id_size) {
        memcpy(engine_session_id, transport_session_id, session_id_len + 1U);
        return engine_session_id;
    }

    /* FNV-1a gives the same short key for every message in one XiaoZhi
     * session without relying on the SDK's lossy prefix truncation. */
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < session_id_len; ++i) {
        hash ^= (uint8_t)transport_session_id[i];
        hash *= UINT64_C(1099511628211);
    }

    const int written = snprintf(engine_session_id, engine_session_id_size,
                                 "xiaozhi-%016" PRIX64, hash);
    if (written < 0 || (size_t)written >= engine_session_id_size) {
        return NULL;
    }

    ESP_LOGW(TAG,
             "MCP session id length=%zu exceeds SDK key capacity=%zu; using hashed key",
             session_id_len, engine_session_id_size - 1U);
    return engine_session_id;
}

static esp_err_t esp_xiaozhi_mcp_build_response(const char *session_id,
                                                const uint8_t *payload,
                                                size_t payload_len,
                                                char **response_out)
{
    ESP_RETURN_ON_FALSE(payload && payload_len > 0 && response_out,
                        ESP_ERR_INVALID_ARG, TAG, "Invalid MCP response arguments");

    *response_out = NULL;

    /* Only serialize the small XiaoZhi envelope. The MCP manager already returned
     * a complete JSON-RPC document, so parsing and printing it again needlessly
     * duplicates the tool list and creates a large transient allocation. */
    cJSON *envelope = cJSON_CreateObject();
    ESP_RETURN_ON_FALSE(envelope, ESP_ERR_NO_MEM, TAG, "Failed to create MCP envelope");

    if (session_id && !cJSON_AddStringToObject(envelope, "session_id", session_id)) {
        cJSON_Delete(envelope);
        return ESP_ERR_NO_MEM;
    }
    if (!cJSON_AddStringToObject(envelope, "type", "mcp")) {
        cJSON_Delete(envelope);
        return ESP_ERR_NO_MEM;
    }

    char *prefix = cJSON_PrintUnformatted(envelope);
    cJSON_Delete(envelope);
    ESP_RETURN_ON_FALSE(prefix, ESP_ERR_NO_MEM, TAG, "Failed to print MCP envelope");

    size_t prefix_len = strlen(prefix);
    static const char payload_field[] = ",\"payload\":";
    const size_t payload_field_len = sizeof(payload_field) - 1;
    if (prefix_len < 1 || prefix[prefix_len - 1] != '}' ||
            payload_len > SIZE_MAX - (prefix_len - 1) - payload_field_len - 1) {
        cJSON_free(prefix);
        return ESP_ERR_INVALID_SIZE;
    }

    const size_t response_len = (prefix_len - 1) + payload_field_len + payload_len + 1;
    char *response = heap_caps_malloc(response_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!response) {
        cJSON_free(prefix);
        return ESP_ERR_NO_MEM;
    }

    memcpy(response, prefix, prefix_len - 1);
    memcpy(response + prefix_len - 1, payload_field, payload_field_len);
    memcpy(response + prefix_len - 1 + payload_field_len, payload, payload_len);
    response[response_len - 1] = '}';
    response[response_len] = '\0';

    cJSON_free(prefix);
    *response_out = response;
    return ESP_OK;
}

static bool esp_xiaozhi_mcp_is_initialize_request(const char *payload, size_t payload_len)
{
    cJSON *request = cJSON_ParseWithLength(payload, payload_len);
    if (!request) {
        return false;
    }

    const cJSON *method = cJSON_GetObjectItem(request, "method");
    const bool is_initialize = cJSON_IsString(method) &&
                               strcmp(method->valuestring, "initialize") == 0;
    cJSON_Delete(request);
    return is_initialize;
}

static esp_xiaozhi_mcp_item_t *esp_xiaozhi_mcp_get_item_by_mgr_handle(esp_mcp_mgr_handle_t mgr_handle)
{
    esp_xiaozhi_mcp_item_t *mcp_item = (esp_xiaozhi_mcp_item_t *)s_xiaozhi_mcp;
    if (mcp_item && mcp_item->handle == mgr_handle) {
        return mcp_item;
    }

    return NULL;
}

esp_err_t esp_xiaozhi_mcp_handle_message(esp_mcp_mgr_handle_t mgr_handle,
                                         const char *ep_name,
                                         const char *payload,
                                         size_t payload_len,
                                         const char *session_id)
{
    ESP_RETURN_ON_FALSE(ep_name, ESP_ERR_INVALID_ARG, TAG, "Invalid endpoint name");
    ESP_RETURN_ON_FALSE(payload, ESP_ERR_INVALID_ARG, TAG, "Invalid payload");

    esp_xiaozhi_mcp_item_t *mcp_item = esp_xiaozhi_mcp_get_item_by_mgr_handle(mgr_handle);
    ESP_RETURN_ON_FALSE(mcp_item, ESP_ERR_INVALID_ARG, TAG, "Invalid MCP item or handle mismatch");

    const char *request_session_id = (session_id && session_id[0]) ? session_id : NULL;
    char engine_session_id[ESP_XIAOZHI_MCP_ENGINE_SESSION_BUFFER_SIZE] = {0};
    const char *engine_session_id_ptr = esp_xiaozhi_mcp_make_engine_session_id(
        request_session_id, engine_session_id, sizeof(engine_session_id));
    ESP_RETURN_ON_FALSE(engine_session_id_ptr, ESP_ERR_INVALID_SIZE, TAG,
                        "Failed to canonicalize MCP session id");

    const bool is_initialize = esp_xiaozhi_mcp_is_initialize_request(payload, payload_len);
    if (is_initialize) {
        esp_err_t clear_ret = esp_mcp_mgr_clear_session_state(mgr_handle, engine_session_id_ptr);
        if (clear_ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to reset MCP session key %s: %s",
                     engine_session_id_ptr, esp_err_to_name(clear_ret));
            return clear_ret;
        }
    }

    esp_err_t context_ret = esp_mcp_mgr_set_request_context(
        mgr_handle, engine_session_id_ptr, NULL, true);
    if (context_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set MCP request context: %s",
                 esp_err_to_name(context_ret));
        return context_ret;
    }

    uint8_t *outbuf = NULL;
    uint16_t outlen = 0;
    esp_err_t request_ret = esp_mcp_mgr_req_handle(
        mgr_handle, ep_name, (const uint8_t *)payload, payload_len, &outbuf, &outlen);
    if (request_ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "MCP handler returned %s (outlen=%u, outer_session_len=%u, engine_session=%s)",
                 esp_err_to_name(request_ret), (unsigned)outlen,
                 (unsigned)(request_session_id ? strlen(request_session_id) : 0U),
                 engine_session_id_ptr);
    }
    esp_err_t ret = request_ret;
    if (outbuf && outlen > 0) {
        char *json_str = NULL;
        ret = esp_xiaozhi_mcp_build_response(request_session_id, outbuf, outlen, &json_str);
        if (ret == ESP_OK) {
            const unsigned response_len = (unsigned)strlen(json_str);
            /* tools/list is multi-KiB and is what registers the device's MCP
             * capabilities with the server. Log it at INFO so a missing tool
             * (the assistant answering "I can't do that") can be traced to a
             * failed/oversized discovery response instead of guessed. */
            if (response_len > 512U) {
                ESP_LOGI(TAG, "Sending MCP response (%u bytes)", response_len);
            } else {
                ESP_LOGD(TAG, "Sending MCP response (%u bytes)", response_len);
            }
            esp_err_t send_ret = esp_xiaozhi_transport_send_text(mcp_item->transport_handle, json_str);
            heap_caps_free(json_str);
            ret = send_ret;
            if (send_ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to send MCP response: %s", esp_err_to_name(send_ret));
            } else if (response_len > 512U) {
                ESP_LOGI(TAG, "MCP response sent (%u bytes)", response_len);
            }
        } else {
            ESP_LOGE(TAG, "Failed to build MCP response: %s", esp_err_to_name(ret));
        }
        esp_mcp_mgr_req_destroy_response(mgr_handle, outbuf);
    } else if (request_ret != ESP_OK && request_ret != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "Failed to process MCP message: %s", esp_err_to_name(request_ret));
    }

    return ret;
}

esp_err_t esp_xiaozhi_mcp_get_handle(esp_mcp_mgr_handle_t mgr_handle, esp_xiaozhi_transport_handle_t *transport_handle)
{
    ESP_RETURN_ON_FALSE(transport_handle, ESP_ERR_INVALID_ARG, TAG, "Invalid transport handle");

    esp_xiaozhi_mcp_item_t *mcp_item = esp_xiaozhi_mcp_get_item_by_mgr_handle(mgr_handle);
    ESP_RETURN_ON_FALSE(mcp_item, ESP_ERR_INVALID_ARG, TAG, "Invalid MCP item or handle mismatch");

    *transport_handle = mcp_item->transport_handle;

    return ESP_OK;
}

static esp_err_t esp_xiaozhi_mcp_init(esp_mcp_mgr_handle_t handle, esp_mcp_transport_handle_t *transport_handle)
{
    esp_xiaozhi_mcp_item_t *mcp_item = calloc(1, sizeof(esp_xiaozhi_mcp_item_t));
    ESP_RETURN_ON_FALSE(mcp_item, ESP_ERR_NO_MEM, TAG, "Failed to allocate memory for MCP transport item");

    mcp_item->handle = handle;
    mcp_item->config = NULL;

    esp_err_t ret = esp_xiaozhi_transport_init(&mcp_item->transport_handle);
    if (ret != ESP_OK) {
        free(mcp_item);
        return ret;
    }

    *transport_handle = (esp_mcp_transport_handle_t)mcp_item;
    s_xiaozhi_mcp = mcp_item;

    return ESP_OK;
}

static esp_err_t esp_xiaozhi_mcp_deinit(esp_mcp_transport_handle_t handle)
{
    esp_xiaozhi_mcp_item_t *mcp_item = (esp_xiaozhi_mcp_item_t *)handle;
    ESP_RETURN_ON_FALSE(mcp_item, ESP_ERR_INVALID_ARG, TAG, "Invalid handle");

    esp_err_t ret = esp_xiaozhi_transport_deinit(mcp_item->transport_handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to deinit MCP transport: %s", esp_err_to_name(ret));
        return ret;
    }
    mcp_item->handle = 0;
    mcp_item->config = NULL;
    if (s_xiaozhi_mcp == mcp_item) {
        s_xiaozhi_mcp = NULL;
    }
    free(mcp_item);

    return ESP_OK;
}

static esp_err_t esp_xiaozhi_mcp_create_config(const void *config, void **config_out)
{
    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "Invalid configuration");

    esp_xiaozhi_mcp_config_t *old_config = (esp_xiaozhi_mcp_config_t *)config;
    esp_xiaozhi_mcp_config_t *new_config = calloc(1, sizeof(esp_xiaozhi_mcp_config_t));
    ESP_RETURN_ON_FALSE(new_config, ESP_ERR_NO_MEM, TAG, "Failed to allocate memory for new configuration");

    new_config->ctx = old_config->ctx;
    new_config->use_mqtt = old_config->use_mqtt;
    new_config->callbacks = old_config->callbacks;

    if (old_config->session_id && old_config->session_id_len > 0) {
        new_config->session_id_len = old_config->session_id_len;
        new_config->session_id = calloc(1, new_config->session_id_len + 1);
        ESP_RETURN_ON_FALSE(new_config->session_id, ESP_ERR_NO_MEM, TAG, "Failed to allocate memory for session ID");
        memcpy(new_config->session_id, old_config->session_id, new_config->session_id_len);
    }

    *config_out = new_config;

    return ESP_OK;
}

static esp_err_t esp_xiaozhi_mcp_delete_config(void *config)
{
    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "Invalid configuration");

    esp_xiaozhi_mcp_config_t *old_config = (esp_xiaozhi_mcp_config_t *)config;
    if (old_config->session_id) {
        free(old_config->session_id);
    }
    free(old_config);

    return ESP_OK;
}

static esp_err_t esp_xiaozhi_mcp_start(esp_mcp_transport_handle_t handle, void *config)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "Invalid handle");
    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "Invalid configuration");

    esp_xiaozhi_mcp_item_t *mcp_item = (esp_xiaozhi_mcp_item_t *)handle;
    esp_xiaozhi_mcp_config_t *new_config = (esp_xiaozhi_mcp_config_t *)config;

    mcp_item->config = new_config;

    esp_err_t ret = esp_xiaozhi_transport_start(mcp_item->transport_handle, new_config->use_mqtt, &new_config->callbacks);
    ESP_RETURN_ON_ERROR(ret, TAG, "Failed to start transport: %s", esp_err_to_name(ret));

    ESP_LOGD(TAG, "MCP transport started with %s", new_config->use_mqtt ? "MQTT" : "WebSocket");

    return ESP_OK;
}

static esp_err_t esp_xiaozhi_mcp_stop(esp_mcp_transport_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "Invalid handle");

    esp_xiaozhi_mcp_item_t *mcp_item = (esp_xiaozhi_mcp_item_t *)handle;

    esp_err_t ret = esp_xiaozhi_transport_stop(mcp_item->transport_handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to stop MCP transport: %s", esp_err_to_name(ret));
        return ret;
    }
    mcp_item->config = NULL;

    ESP_LOGD(TAG, "MCP transport stopped");

    return ESP_OK;
}

static esp_err_t esp_xiaozhi_mcp_register_endpoint(esp_mcp_transport_handle_t handle, const char *ep_name, void *priv_data)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "Invalid handle");
    ESP_RETURN_ON_FALSE(ep_name, ESP_ERR_INVALID_ARG, TAG, "Invalid endpoint name");

    ESP_LOGD(TAG, "MCP endpoint registered: %s", ep_name);

    return ESP_OK;
}

static esp_err_t esp_xiaozhi_mcp_unregister_endpoint(esp_mcp_transport_handle_t handle, const char *ep_name)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "Invalid handle");
    ESP_RETURN_ON_FALSE(ep_name, ESP_ERR_INVALID_ARG, TAG, "Invalid endpoint name");

    ESP_LOGD(TAG, "MCP endpoint unregistered: %s", ep_name);

    return ESP_OK;
}

const esp_mcp_transport_t esp_mcp_transport_xiaozhi = {
    .init                = esp_xiaozhi_mcp_init,
    .deinit              = esp_xiaozhi_mcp_deinit,
    .create_config       = esp_xiaozhi_mcp_create_config,
    .delete_config       = esp_xiaozhi_mcp_delete_config,
    .start               = esp_xiaozhi_mcp_start,
    .stop                = esp_xiaozhi_mcp_stop,
    .register_endpoint   = esp_xiaozhi_mcp_register_endpoint,
    .unregister_endpoint = esp_xiaozhi_mcp_unregister_endpoint,
};
