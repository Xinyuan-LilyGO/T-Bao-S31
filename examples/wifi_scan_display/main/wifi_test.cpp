#include "wifi_test.h"

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

constexpr char kTag[] = "wifi_test";
constexpr size_t kMaxScanRecords = 48;
constexpr int kScanIntervalMs = 10000;
constexpr int kScanFailureDelayMs = 3000;
constexpr int kConnectTimeoutMs = 12000;
constexpr int kConnectRetryDelayMs = 1000;
constexpr int kMaxConnectAttempts = 3;
constexpr EventBits_t kGotIpBit = BIT0;
constexpr EventBits_t kDisconnectedBit = BIT1;

struct ScanResult {
    bool target_found = false;
    wifi_ap_record_t target = {};
};

EventGroupHandle_t s_wifi_events = nullptr;
esp_netif_t *s_sta_netif = nullptr;
WifiStateCallback s_state_callback = nullptr;
void *s_state_context = nullptr;
std::atomic<uint8_t> s_disconnect_reason{0};

void publish_state(const WifiTestState &state)
{
    if (s_state_callback != nullptr) {
        s_state_callback(state, s_state_context);
    }
}

bool credentials_configured()
{
    return WIFI_TEST_TARGET_SSID[0] != '\0';
}

bool ssid_equals(const uint8_t *ssid, const char *expected)
{
    return std::strncmp(
               reinterpret_cast<const char *>(ssid), expected, 33) == 0;
}

bool state_contains_ssid(const WifiTestState &state, const char *ssid)
{
    for (size_t i = 0; i < state.visible_ap_count; ++i) {
        if (std::strncmp(state.access_points[i].ssid, ssid, 33) == 0) {
            return true;
        }
    }
    return false;
}

WifiAccessPoint make_display_ap(const wifi_ap_record_t &record)
{
    WifiAccessPoint result = {};
    std::memcpy(result.ssid, record.ssid, 32);
    result.ssid[32] = '\0';
    result.rssi = record.rssi;
    result.channel = record.primary;
    result.secured = record.authmode != WIFI_AUTH_OPEN;
    result.is_target =
        credentials_configured() && ssid_equals(record.ssid, WIFI_TEST_TARGET_SSID);
    return result;
}

void add_display_ap(WifiTestState *state, const wifi_ap_record_t &record)
{
    const WifiAccessPoint display_ap = make_display_ap(record);
    if (display_ap.ssid[0] == '\0' ||
        state_contains_ssid(*state, display_ap.ssid)) {
        return;
    }
    if (state->visible_ap_count >= kWifiDisplayApLimit) {
        return;
    }
    state->access_points[state->visible_ap_count++] = display_ap;
}

void ensure_target_is_visible(
    WifiTestState *state,
    const wifi_ap_record_t &target)
{
    if (state_contains_ssid(
            *state, reinterpret_cast<const char *>(target.ssid))) {
        return;
    }

    const WifiAccessPoint target_ap = make_display_ap(target);
    if (state->visible_ap_count < kWifiDisplayApLimit) {
        state->access_points[state->visible_ap_count++] = target_ap;
    } else {
        state->access_points[kWifiDisplayApLimit - 1] = target_ap;
    }
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
        return "access point not found";
    case WIFI_REASON_ASSOC_FAIL:
        return "association failed";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return "handshake timeout";
    case WIFI_REASON_BEACON_TIMEOUT:
        return "beacon timeout";
    case WIFI_REASON_CONNECTION_FAIL:
        return "connection failed";
    default:
        return "link lost";
    }
}

void wifi_event_handler(
    void *,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const auto *event =
            static_cast<const wifi_event_sta_disconnected_t *>(event_data);
        s_disconnect_reason.store(
            event != nullptr ? event->reason : 0, std::memory_order_relaxed);
        xEventGroupClearBits(s_wifi_events, kGotIpBit);
        xEventGroupSetBits(s_wifi_events, kDisconnectedBit);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupClearBits(s_wifi_events, kDisconnectedBit);
        xEventGroupSetBits(s_wifi_events, kGotIpBit);
    }
}

esp_err_t scan_start_with_retry(const wifi_scan_config_t *scan_config)
{
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < 3; ++attempt) {
        err = esp_wifi_scan_start(scan_config, true);
        if (err != ESP_ERR_WIFI_STATE) {
            return err;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    return err;
}

esp_err_t scan_networks(
    WifiTestState *state,
    ScanResult *scan_result,
    wifi_ap_record_t *records)
{
    *state = {};
    *scan_result = {};
    state->phase = WifiTestPhase::kScanning;
    std::snprintf(
        state->detail, sizeof(state->detail), "Scanning nearby networks...");
    publish_state(*state);

    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(150));
    xEventGroupClearBits(s_wifi_events, kGotIpBit | kDisconnectedBit);

    wifi_scan_config_t scan_config = {};
    scan_config.show_hidden = false;
    scan_config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    ESP_RETURN_ON_ERROR(
        scan_start_with_retry(&scan_config), kTag, "start Wi-Fi scan");

    uint16_t total_count = 0;
    ESP_RETURN_ON_ERROR(
        esp_wifi_scan_get_ap_num(&total_count), kTag, "get AP count");
    state->total_ap_count = total_count;

    if (total_count == 0) {
        esp_wifi_clear_ap_list();
        state->phase = credentials_configured()
                           ? WifiTestPhase::kTargetNotFound
                           : WifiTestPhase::kTargetNotConfigured;
        std::snprintf(
            state->detail,
            sizeof(state->detail),
            credentials_configured() ? "No networks found; rescan in 10s"
                                     : "Credentials not configured");
        publish_state(*state);
        return ESP_OK;
    }

    uint16_t record_count = static_cast<uint16_t>(
        std::min<size_t>(total_count, kMaxScanRecords));
    std::memset(records, 0, sizeof(*records) * kMaxScanRecords);
    const esp_err_t records_err =
        esp_wifi_scan_get_ap_records(&record_count, records);
    if (records_err != ESP_OK) {
        esp_wifi_clear_ap_list();
        return records_err;
    }

    std::sort(
        records,
        records + record_count,
        [](const wifi_ap_record_t &left, const wifi_ap_record_t &right) {
            return left.rssi > right.rssi;
        });

    ESP_LOGI(
        kTag,
        "Scan found %u APs; processing strongest %u",
        total_count,
        record_count);
    for (uint16_t i = 0; i < record_count; ++i) {
        ESP_LOGI(
            kTag,
            "AP %u: ssid='%s' rssi=%d channel=%u auth=%d",
            i + 1,
            records[i].ssid,
            records[i].rssi,
            records[i].primary,
            records[i].authmode);
        add_display_ap(state, records[i]);

        if (!scan_result->target_found && credentials_configured() &&
            ssid_equals(records[i].ssid, WIFI_TEST_TARGET_SSID)) {
            scan_result->target_found = true;
            scan_result->target = records[i];
        }
    }

    if (!credentials_configured()) {
        state->phase = WifiTestPhase::kTargetNotConfigured;
        std::snprintf(
            state->detail,
            sizeof(state->detail),
            "Credentials not configured; scan only");
        publish_state(*state);
        return ESP_OK;
    }

    if (!scan_result->target_found) {
        state->phase = WifiTestPhase::kTargetNotFound;
        std::snprintf(
            state->detail,
            sizeof(state->detail),
            "Target not found; rescan in 10s");
        publish_state(*state);
        return ESP_OK;
    }

    ensure_target_is_visible(state, scan_result->target);
    return ESP_OK;
}

esp_err_t set_target_config(const wifi_ap_record_t &target)
{
    wifi_config_t config = {};
    const size_t ssid_length = std::strlen(WIFI_TEST_TARGET_SSID);
    const size_t password_length = std::strlen(WIFI_TEST_TARGET_PASSWORD);
    std::memcpy(config.sta.ssid, WIFI_TEST_TARGET_SSID, ssid_length);
    std::memcpy(
        config.sta.password, WIFI_TEST_TARGET_PASSWORD, password_length);
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
    xEventGroupClearBits(s_wifi_events, kGotIpBit | kDisconnectedBit);
    return esp_wifi_set_config(WIFI_IF_STA, &config);
}

bool connect_to_target(
    const wifi_ap_record_t &target,
    WifiTestState *state)
{
    const esp_err_t config_err = set_target_config(target);
    if (config_err != ESP_OK) {
        state->phase = WifiTestPhase::kConnectionFailed;
        std::snprintf(
            state->detail,
            sizeof(state->detail),
            "Wi-Fi config failed: %s",
            esp_err_to_name(config_err));
        publish_state(*state);
        return false;
    }

    uint8_t last_reason = 0;
    for (int attempt = 1; attempt <= kMaxConnectAttempts; ++attempt) {
        state->phase = WifiTestPhase::kConnecting;
        std::snprintf(
            state->detail,
            sizeof(state->detail),
            "Connecting %s (%d/%d)",
            WIFI_TEST_TARGET_SSID,
            attempt,
            kMaxConnectAttempts);
        publish_state(*state);

        xEventGroupClearBits(s_wifi_events, kGotIpBit | kDisconnectedBit);
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
            s_wifi_events,
            kGotIpBit | kDisconnectedBit,
            pdFALSE,
            pdFALSE,
            pdMS_TO_TICKS(kConnectTimeoutMs));
        if ((bits & kGotIpBit) != 0 && (bits & kDisconnectedBit) == 0) {
            xEventGroupClearBits(s_wifi_events, kGotIpBit);
            esp_netif_ip_info_t ip_info = {};
            if (esp_netif_get_ip_info(s_sta_netif, &ip_info) == ESP_OK) {
                std::snprintf(
                    state->detail,
                    sizeof(state->detail),
                    "Connected " IPSTR,
                    IP2STR(&ip_info.ip));
                ESP_LOGI(
                    kTag,
                    "Connected to '%s', IP=" IPSTR,
                    WIFI_TEST_TARGET_SSID,
                    IP2STR(&ip_info.ip));
            } else {
                std::snprintf(
                    state->detail, sizeof(state->detail), "Connected to target");
                ESP_LOGI(kTag, "Connected to '%s'", WIFI_TEST_TARGET_SSID);
            }
            state->phase = WifiTestPhase::kConnected;
            publish_state(*state);
            return true;
        }

        if ((bits & kDisconnectedBit) != 0) {
            last_reason =
                s_disconnect_reason.load(std::memory_order_relaxed);
            xEventGroupClearBits(s_wifi_events, kDisconnectedBit);
            ESP_LOGW(
                kTag,
                "Connect attempt %d disconnected: reason=%u (%s)",
                attempt,
                last_reason,
                disconnect_reason_text(last_reason));
        } else {
            ESP_LOGW(kTag, "Connect attempt %d timed out", attempt);
        }

        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(kConnectRetryDelayMs));
    }

    state->phase = WifiTestPhase::kConnectionFailed;
    if (last_reason != 0) {
        std::snprintf(
            state->detail,
            sizeof(state->detail),
            "Connect failed: %s",
            disconnect_reason_text(last_reason));
    } else {
        std::snprintf(
            state->detail, sizeof(state->detail), "Connect failed: timeout");
    }
    publish_state(*state);
    return false;
}

void wait_for_disconnect(WifiTestState *state)
{
    xEventGroupWaitBits(
        s_wifi_events,
        kDisconnectedBit,
        pdTRUE,
        pdFALSE,
        portMAX_DELAY);
    const uint8_t reason =
        s_disconnect_reason.load(std::memory_order_relaxed);
    state->phase = WifiTestPhase::kDisconnected;
    std::snprintf(
        state->detail,
        sizeof(state->detail),
        "Disconnected: %s",
        disconnect_reason_text(reason));
    publish_state(*state);
    ESP_LOGW(
        kTag,
        "Disconnected from '%s': reason=%u (%s)",
        WIFI_TEST_TARGET_SSID,
        reason,
        disconnect_reason_text(reason));
}

void wifi_task(void *)
{
    auto *records = static_cast<wifi_ap_record_t *>(
        std::calloc(kMaxScanRecords, sizeof(wifi_ap_record_t)));
    if (records == nullptr) {
        WifiTestState state = {};
        state.phase = WifiTestPhase::kConnectionFailed;
        std::snprintf(
            state.detail, sizeof(state.detail), "No memory for Wi-Fi scan");
        publish_state(state);
        vTaskDelete(nullptr);
        return;
    }

    WifiTestState state = {};
    while (true) {
        ScanResult scan_result = {};
        const esp_err_t scan_err =
            scan_networks(&state, &scan_result, records);
        if (scan_err != ESP_OK) {
            state.phase = WifiTestPhase::kConnectionFailed;
            std::snprintf(
                state.detail,
                sizeof(state.detail),
                "Scan failed: %s",
                esp_err_to_name(scan_err));
            publish_state(state);
            ESP_LOGE(kTag, "%s", state.detail);
            vTaskDelay(pdMS_TO_TICKS(kScanFailureDelayMs));
            continue;
        }

        if (!scan_result.target_found) {
            vTaskDelay(pdMS_TO_TICKS(kScanIntervalMs));
            continue;
        }

        if (connect_to_target(scan_result.target, &state)) {
            wait_for_disconnect(&state);
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

esp_err_t wifi_test_start(WifiStateCallback callback, void *context)
{
    s_state_callback = callback;
    s_state_context = context;

    WifiTestState initial_state = {};
    initial_state.phase = WifiTestPhase::kInitializing;
    std::snprintf(
        initial_state.detail,
        sizeof(initial_state.detail),
        "Initializing Wi-Fi...");
    publish_state(initial_state);

    s_wifi_events = xEventGroupCreate();
    if (s_wifi_events == nullptr) {
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
        esp_netif_set_hostname(s_sta_netif, "t-bao-s31"),
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
            wifi_event_handler,
            nullptr,
            &wifi_handler),
        kTag,
        "register Wi-Fi event handler");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(
            IP_EVENT,
            IP_EVENT_STA_GOT_IP,
            wifi_event_handler,
            nullptr,
            &ip_handler),
        kTag,
        "register IP event handler");

    ESP_RETURN_ON_ERROR(
        esp_wifi_set_mode(WIFI_MODE_STA), kTag, "set station mode");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), kTag, "start Wi-Fi");
    ESP_RETURN_ON_ERROR(
        esp_wifi_set_ps(WIFI_PS_NONE), kTag, "disable Wi-Fi power save");

    if (xTaskCreate(
            wifi_task,
            "wifi_test",
            8192,
            nullptr,
            5,
            nullptr) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
