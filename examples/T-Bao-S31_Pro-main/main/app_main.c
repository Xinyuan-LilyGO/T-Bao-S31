/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <esp_log.h>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <esp_event.h>
#include <esp_wifi.h>
#include <cJSON.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_xiaozhi_chat_app.h"

static const char *TAG = "app_main";

/* MCP JSON is the largest transient heap user on this board: tools/list
 * serializes every registered tool (the SDK caps one page at 8 KiB) and the
 * MCP engine parses that text into a fresh cJSON tree before replying. With
 * cJSON on the internal heap that tree needs tens of KiB and exhausts the
 * ~14 KiB left after Wi-Fi/AFE/display, so the receive path fails with
 * ENOMEM right after tools/list (transport_ws read -2, errno 12) and the
 * session dies before any answer is played. cJSON memory is never DMA or
 * cache-disabled critical, so keep it in PSRAM. */
static void *app_cjson_psram_malloc(size_t size)
{
    void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ptr == NULL) {
        ptr = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return ptr;
}

static void app_cjson_psram_free(void *ptr)
{
    heap_caps_free(ptr);
}

void app_main(void)
{
    cJSON_Hooks cjson_hooks = {
        .malloc_fn = app_cjson_psram_malloc,
        .free_fn = app_cjson_psram_free,
    };
    cJSON_InitHooks(&cjson_hooks);

    /* Record why the previous boot ended. A wake-time reboot is either a
     * panic (backtrace is printed by the panic handler) or a brownout/power
     * event; this line makes the two distinguishable in the next log. */
    ESP_LOGI(TAG, "reset reason: %d", (int)esp_reset_reason());

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* WiFi is owned by the chat app now: it connects to stored credentials
     * and, if none are configured, enters SoftAP provisioning (the board
     * broadcasts its own AP + a captive-portal config page). We must NOT call
     * example_connect() here too, or wifi would be initialized twice. */

    /* The chat app brings up the board (LCD/emote), connects to the xiaozhi AI
     * service, and (if the audio codec is reachable) voice. With the board
     * adapter now tolerating a missing codec and the chat app retrying on
     * network failure, the device stays alive (screen lit) even when audio
     * hardware or WiFi are temporarily unavailable. */
    esp_xiaozhi_chat_app();
}
