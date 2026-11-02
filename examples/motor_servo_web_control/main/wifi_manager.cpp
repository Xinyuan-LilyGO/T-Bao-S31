#include "wifi_manager.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#if __has_include("wifi_credentials.h")
#include "wifi_credentials.h"
#endif

#ifndef WIFI_TEST_TARGET_SSID
#define WIFI_TEST_TARGET_SSID ""
#endif

#ifndef WIFI_TEST_TARGET_PASSWORD
#define WIFI_TEST_TARGET_PASSWORD ""
#endif

static_assert(sizeof(WIFI_TEST_TARGET_SSID) - 1 <= 32, "Wi-Fi SSID is too long");
static_assert(
    sizeof(WIFI_TEST_TARGET_PASSWORD) - 1 <= 63,
    "Wi-Fi password is too long");

namespace {

constexpr char kTag[] = "wifi_manager";
constexpr size_t kMaxScanRecords = 64;
constexpr int kScanIntervalMs = 5000;
constexpr int kFailureDelayMs = 2500;
constexpr int kConnectTimeoutMs = 12000;
constexpr int kConnectRetryDelayMs = 1000;
constexpr int kMaxConnectAttempts = 3;
constexpr EventBits_t kGotIpBit = BIT0;
constexpr EventBits_t kDisconnectedBit = BIT1;

EventGroupHandle_t s_events = nullptr;
esp_netif_t *s_sta_netif = nullptr;
WifiStatusCallback s_callback = nullptr;
void *s_callback_context = nullptr;
std::atomic<uint8_t> s_disconnect_reason{0};

struct ScanResult {
    bool target_found = false;
    wifi_ap_record_t target = {};
};

void publish(const WifiStatus &status)
{
    if (s_callback != nullptr) {
        s_callback(status, s_callback_context);
    }
}

bool credentials_configured()
{
    return WIFI_TEST_TARGET_SSID[0] != '\0';
}

bool ssid_matches(const uint8_t *ssid)
{
    return std::strncmp(
               reinterpret_cast<const char *>(ssid),
               WIFI_TEST_TARGET_SSID,
               33) == 0;
}

const char *disconnect_reason_text(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_AUTH_FAIL:
        return "authentication failed";
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return "target access point not found";
    case WIFI_REASON_ASSOC_FAIL:
        return "association failed";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return "handshake timeout";
    case WIFI_REASON_BEACON_TIMEOUT:
        return "beacon timeout";
    default:
        return "link lost";
    }
}

void event_handler(
    void *,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const auto *event =
            static_cast<const wifi_event_sta_disconnected_t *>(event_data);
        s_disconnect_reason.store(
            event != nullptr ? event->reason : 0,
            std::memory_order_relaxed);
        xEventGroupClearBits(s_events, kGotIpBit);
        xEventGroupSetBits(s_events, kDisconnectedBit);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupClearBits(s_events, kDisconnectedBit);
        xEventGroupSetBits(s_events, kGotIpBit);
    }
}

esp_err_t start_scan_with_retry(const wifi_scan_config_t *config)
{
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < 3; ++attempt) {
        err = esp_wifi_scan_start(config, true);
        if (err != ESP_ERR_WIFI_STATE) {
            return err;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    return err;
}

esp_err_t scan_networks(WifiStatus *status, ScanResult *result)
{
    *status = {};
    *result = {};
    status->phase = WifiPhase::kScanning;
    std::snprintf(status->detail, sizeof(status->detail), "Scanning nearby Wi-Fi...");
    publish(*status);

    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(150));
    xEventGroupClearBits(s_events, kGotIpBit | kDisconnectedBit);

    wifi_scan_config_t scan_config = {};
    scan_config.show_hidden = false;
    scan_config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    ESP_RETURN_ON_ERROR(
        start_scan_with_retry(&scan_config), kTag, "start Wi-Fi scan");

    uint16_t total_count = 0;
    ESP_RETURN_ON_ERROR(
        esp_wifi_scan_get_ap_num(&total_count), kTag, "get AP count");
    status->ap_count = total_count;

    if (total_count == 0) {
        esp_wifi_clear_ap_list();
        status->phase = credentials_configured()
                            ? WifiPhase::kTargetNotFound
                            : WifiPhase::kTargetNotConfigured;
        std::snprintf(
            status->detail,
            sizeof(status->detail),
            credentials_configured() ? "No Wi-Fi found; scanning again"
                                     : "Local Wi-Fi credentials are missing");
        publish(*status);
        return ESP_OK;
    }

    const uint16_t capacity = static_cast<uint16_t>(
        std::min<size_t>(total_count, kMaxScanRecords));
    auto *records = static_cast<wifi_ap_record_t *>(
        std::calloc(capacity, sizeof(wifi_ap_record_t)));
    if (records == nullptr) {
        esp_wifi_clear_ap_list();
        return ESP_ERR_NO_MEM;
    }

    uint16_t record_count = capacity;
    const esp_err_t records_err =
        esp_wifi_scan_get_ap_records(&record_count, records);
    if (records_err != ESP_OK) {
        std::free(records);
        esp_wifi_clear_ap_list();
        return records_err;
    }

    for (uint16_t i = 0; i < record_count; ++i) {
        if (credentials_configured() && ssid_matches(records[i].ssid)) {
            if (!result->target_found ||
                records[i].rssi > result->target.rssi) {
                result->target_found = true;
                result->target = records[i];
            }
        }
    }
    std::free(records);

    status->target_found = result->target_found;
    if (result->target_found) {
        status->target_rssi = result->target.rssi;
        std::snprintf(
            status->detail,
            sizeof(status->detail),
            "Target found, RSSI %d dBm",
            result->target.rssi);
        publish(*status);
        return ESP_OK;
    }

    status->phase = credentials_configured()
                        ? WifiPhase::kTargetNotFound
                        : WifiPhase::kTargetNotConfigured;
    std::snprintf(
        status->detail,
        sizeof(status->detail),
        credentials_configured() ? "Target not found; scanning again"
                                 : "Local Wi-Fi credentials are missing");
    publish(*status);
    return ESP_OK;
}

esp_err_t configure_target(const wifi_ap_record_t &target)
{
    wifi_config_t config = {};
    const size_t ssid_length = std::strlen(WIFI_TEST_TARGET_SSID);
    const size_t password_length = std::strlen(WIFI_TEST_TARGET_PASSWORD);
    std::memcpy(config.sta.ssid, WIFI_TEST_TARGET_SSID, ssid_length);
    std::memcpy(config.sta.password, WIFI_TEST_TARGET_PASSWORD, password_length);
    config.sta.scan_method = WIFI_FAST_SCAN;
    config.sta.bssid_set = true;
    std::memcpy(config.sta.bssid, target.bssid, sizeof(config.sta.bssid));
    config.sta.channel = target.primary;
    config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    config.sta.threshold.rssi = -127;
    config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    config.sta.pmf_cfg.capable = true;
    config.sta.pmf_cfg.required = false;
    config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(150));
    xEventGroupClearBits(s_events, kGotIpBit | kDisconnectedBit);
    return esp_wifi_set_config(WIFI_IF_STA, &config);
}

bool connect_target(const wifi_ap_record_t &target, WifiStatus *status)
{
    const esp_err_t config_err = configure_target(target);
    if (config_err != ESP_OK) {
        status->phase = WifiPhase::kConnectionFailed;
        std::snprintf(
            status->detail,
            sizeof(status->detail),
            "Wi-Fi config failed: %s",
            esp_err_to_name(config_err));
        publish(*status);
        return false;
    }

    uint8_t last_reason = 0;
    for (int attempt = 1; attempt <= kMaxConnectAttempts; ++attempt) {
        status->phase = WifiPhase::kConnecting;
        std::snprintf(
            status->detail,
            sizeof(status->detail),
            "Connecting to target (%d/%d)",
            attempt,
            kMaxConnectAttempts);
        publish(*status);

        xEventGroupClearBits(s_events, kGotIpBit | kDisconnectedBit);
        const esp_err_t connect_err = esp_wifi_connect();
        if (connect_err != ESP_OK) {
            ESP_LOGW(
                kTag,
                "Connect attempt %d failed to start: %s",
                attempt,
                esp_err_to_name(connect_err));
            vTaskDelay(pdMS_TO_TICKS(kConnectRetryDelayMs));
            continue;
        }

        const EventBits_t bits = xEventGroupWaitBits(
            s_events,
            kGotIpBit | kDisconnectedBit,
            pdFALSE,
            pdFALSE,
            pdMS_TO_TICKS(kConnectTimeoutMs));
        if ((bits & kGotIpBit) != 0 && (bits & kDisconnectedBit) == 0) {
            esp_netif_ip_info_t ip_info = {};
            if (esp_netif_get_ip_info(s_sta_netif, &ip_info) != ESP_OK) {
                continue;
            }

            status->phase = WifiPhase::kConnected;
            status->target_found = true;
            std::snprintf(
                status->ip_address,
                sizeof(status->ip_address),
                IPSTR,
                IP2STR(&ip_info.ip));
            std::snprintf(
                status->web_url,
                sizeof(status->web_url),
                "http://" IPSTR,
                IP2STR(&ip_info.ip));
            std::snprintf(
                status->detail,
                sizeof(status->detail),
                "Wi-Fi connected; open the URL below");
            ESP_LOGI(kTag, "Control page: %s", status->web_url);
            publish(*status);
            return true;
        }

        if ((bits & kDisconnectedBit) != 0) {
            last_reason =
                s_disconnect_reason.load(std::memory_order_relaxed);
            xEventGroupClearBits(s_events, kDisconnectedBit);
            ESP_LOGW(
                kTag,
                "Connect attempt %d disconnected: reason=%u",
                attempt,
                last_reason);
        }
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(kConnectRetryDelayMs));
    }

    status->phase = WifiPhase::kConnectionFailed;
    std::snprintf(
        status->detail,
        sizeof(status->detail),
        "Connect failed: %s",
        last_reason != 0 ? disconnect_reason_text(last_reason) : "timeout");
    publish(*status);
    return false;
}

void wait_for_disconnect(WifiStatus *status)
{
    xEventGroupWaitBits(
        s_events,
        kDisconnectedBit,
        pdTRUE,
        pdFALSE,
        portMAX_DELAY);
    const uint8_t reason =
        s_disconnect_reason.load(std::memory_order_relaxed);
    status->phase = WifiPhase::kDisconnected;
    status->ip_address[0] = '\0';
    status->web_url[0] = '\0';
    std::snprintf(
        status->detail,
        sizeof(status->detail),
        "Disconnected: %s",
        disconnect_reason_text(reason));
    publish(*status);
}

void wifi_task(void *)
{
    WifiStatus status = {};
    while (true) {
        ScanResult result = {};
        const esp_err_t scan_err = scan_networks(&status, &result);
        if (scan_err != ESP_OK) {
            status.phase = WifiPhase::kConnectionFailed;
            std::snprintf(
                status.detail,
                sizeof(status.detail),
                "Scan failed: %s",
                esp_err_to_name(scan_err));
            publish(status);
            vTaskDelay(pdMS_TO_TICKS(kFailureDelayMs));
            continue;
        }

        if (!result.target_found) {
            vTaskDelay(pdMS_TO_TICKS(kScanIntervalMs));
            continue;
        }

        if (connect_target(result.target, &status)) {
            wait_for_disconnect(&status);
            vTaskDelay(pdMS_TO_TICKS(kConnectRetryDelayMs));
        } else {
            vTaskDelay(pdMS_TO_TICKS(kScanIntervalMs));
        }
    }
}

esp_err_t init_nvs()
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), kTag, "erase NVS");
        err = nvs_flash_init();
    }
    return err;
}

}  // namespace

esp_err_t wifi_manager_start(WifiStatusCallback callback, void *context)
{
    s_callback = callback;
    s_callback_context = context;

    WifiStatus initial = {};
    initial.phase = WifiPhase::kInitializing;
    std::snprintf(
        initial.detail,
        sizeof(initial.detail),
        "Initializing Wi-Fi station...");
    publish(initial);

    s_events = xEventGroupCreate();
    if (s_events == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    ESP_RETURN_ON_ERROR(init_nvs(), kTag, "initialize NVS");
    ESP_RETURN_ON_ERROR(esp_netif_init(), kTag, "initialize esp-netif");
    ESP_RETURN_ON_ERROR(
        esp_event_loop_create_default(), kTag, "create default event loop");

    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (s_sta_netif == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    ESP_RETURN_ON_ERROR(
        esp_netif_set_hostname(s_sta_netif, "t-bao-s31-control"),
        kTag,
        "set station hostname");

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(
        esp_wifi_init(&init_config), kTag, "initialize Wi-Fi driver");
    ESP_RETURN_ON_ERROR(
        esp_wifi_set_storage(WIFI_STORAGE_RAM), kTag, "use RAM Wi-Fi storage");

    esp_event_handler_instance_t wifi_handler = nullptr;
    esp_event_handler_instance_t ip_handler = nullptr;
    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(
            WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            event_handler,
            nullptr,
            &wifi_handler),
        kTag,
        "register Wi-Fi events");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(
            IP_EVENT,
            IP_EVENT_STA_GOT_IP,
            event_handler,
            nullptr,
            &ip_handler),
        kTag,
        "register IP events");

    ESP_RETURN_ON_ERROR(
        esp_wifi_set_mode(WIFI_MODE_STA), kTag, "set station mode");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), kTag, "start Wi-Fi");
    ESP_RETURN_ON_ERROR(
        esp_wifi_set_ps(WIFI_PS_NONE), kTag, "disable Wi-Fi power save");

    if (xTaskCreate(
            wifi_task,
            "wifi_control",
            8192,
            nullptr,
            5,
            nullptr) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
