#include "factory_tests.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "board_service.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "factory_settings.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/ip_addr.h"
#include "ping/ping_sock.h"

static_assert(sizeof(FACTORY_WIFI_SSID) - 1 <= 32, "Factory Wi-Fi SSID is too long");
static_assert(
    sizeof(FACTORY_WIFI_PASSWORD) - 1 <= 63,
    "Factory Wi-Fi password is too long");

namespace {

constexpr char kTag[] = "factory_wifi";
constexpr EventBits_t kGotIpBit = BIT0;
constexpr EventBits_t kDisconnectedBit = BIT1;
constexpr uint32_t kConnectTimeoutMs = 12000;
constexpr int kConnectAttempts = 3;
constexpr uint16_t kMaxScanRecords = 64;

EventGroupHandle_t s_events = nullptr;
esp_netif_t *s_sta_netif = nullptr;
esp_event_handler_instance_t s_wifi_handler = nullptr;
esp_event_handler_instance_t s_ip_handler = nullptr;
esp_ping_handle_t s_ping = nullptr;
SemaphoreHandle_t s_ping_done = nullptr;
bool s_wifi_initialized = false;
bool s_wifi_started = false;
std::atomic<uint8_t> s_disconnect_reason{0};
std::atomic<uint32_t> s_ping_replies{0};
std::atomic<uint32_t> s_ping_rtt_sum_ms{0};

struct ScanTarget {
    bool found = false;
    wifi_ap_record_t ap = {};
    uint16_t total_count = 0;
};

struct PingResult {
    uint32_t transmitted = 0;
    uint32_t received = 0;
    uint32_t duration_ms = 0;
    uint32_t average_rtt_ms = 0;
};

void set_failure(FactoryTestRecord *record, const char *code, const char *detail)
{
    std::snprintf(record->error_code, sizeof(record->error_code), "%s", code);
    std::snprintf(record->detail, sizeof(record->detail), "%s", detail);
}

bool ssid_matches(const uint8_t *ssid)
{
    return std::strncmp(
               reinterpret_cast<const char *>(ssid), FACTORY_WIFI_SSID, 33) == 0;
}

void wifi_event_handler(
    void *, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (s_events == nullptr) {
        return;
    }
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const auto *event =
            static_cast<const wifi_event_sta_disconnected_t *>(event_data);
        s_disconnect_reason.store(
            event != nullptr ? event->reason : 0, std::memory_order_relaxed);
        xEventGroupClearBits(s_events, kGotIpBit);
        xEventGroupSetBits(s_events, kDisconnectedBit);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupClearBits(s_events, kDisconnectedBit);
        xEventGroupSetBits(s_events, kGotIpBit);
    }
}

void ping_success(esp_ping_handle_t handle, void *)
{
    uint32_t elapsed_ms = 0;
    if (esp_ping_get_profile(
            handle, ESP_PING_PROF_TIMEGAP, &elapsed_ms, sizeof(elapsed_ms)) ==
        ESP_OK) {
        s_ping_rtt_sum_ms.fetch_add(elapsed_ms, std::memory_order_relaxed);
    }
    s_ping_replies.fetch_add(1, std::memory_order_relaxed);
}

void ping_timeout(esp_ping_handle_t, void *)
{
}

void ping_end(esp_ping_handle_t, void *)
{
    if (s_ping_done != nullptr) {
        xSemaphoreGive(s_ping_done);
    }
}

esp_err_t init_network_stack()
{
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    return ESP_OK;
}

esp_err_t init_wifi()
{
    ESP_RETURN_ON_ERROR(init_network_stack(), kTag, "initialize network stack");
    s_events = xEventGroupCreate();
    if (s_events == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (s_sta_netif == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    ESP_RETURN_ON_ERROR(
        esp_netif_set_hostname(s_sta_netif, "t-bao-s31-factory"),
        kTag, "set station hostname");

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), kTag, "initialize Wi-Fi");
    s_wifi_initialized = true;
    ESP_RETURN_ON_ERROR(
        esp_wifi_set_storage(WIFI_STORAGE_RAM), kTag, "use RAM Wi-Fi config");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(
            WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, nullptr,
            &s_wifi_handler),
        kTag, "register Wi-Fi events");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(
            IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, nullptr,
            &s_ip_handler),
        kTag, "register IP events");
    ESP_RETURN_ON_ERROR(
        esp_wifi_set_mode(WIFI_MODE_STA), kTag, "set station mode");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), kTag, "start Wi-Fi");
    s_wifi_started = true;
    ESP_RETURN_ON_ERROR(
        esp_wifi_set_ps(WIFI_PS_NONE), kTag, "disable Wi-Fi power save");
    return ESP_OK;
}

esp_err_t scan_target(ScanTarget *target)
{
    if (target == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *target = {};
    wifi_scan_config_t scan = {};
    scan.show_hidden = false;
    scan.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    ESP_RETURN_ON_ERROR(
        esp_wifi_scan_start(&scan, true), kTag, "scan factory SSID");
    ESP_RETURN_ON_ERROR(
        esp_wifi_scan_get_ap_num(&target->total_count), kTag, "get scan count");
    if (target->total_count == 0) {
        esp_wifi_clear_ap_list();
        return ESP_OK;
    }

    uint16_t count = std::min<uint16_t>(target->total_count, kMaxScanRecords);
    std::vector<wifi_ap_record_t> records(count);
    esp_err_t err = esp_wifi_scan_get_ap_records(&count, records.data());
    if (err != ESP_OK) {
        esp_wifi_clear_ap_list();
        return err;
    }
    for (uint16_t i = 0; i < count; ++i) {
        if (ssid_matches(records[i].ssid) &&
            (!target->found || records[i].rssi > target->ap.rssi)) {
            target->found = true;
            target->ap = records[i];
        }
    }
    return ESP_OK;
}

esp_err_t configure_target(const wifi_ap_record_t &target)
{
    wifi_config_t config = {};
    std::memcpy(
        config.sta.ssid, FACTORY_WIFI_SSID, std::strlen(FACTORY_WIFI_SSID));
    std::memcpy(
        config.sta.password, FACTORY_WIFI_PASSWORD,
        std::strlen(FACTORY_WIFI_PASSWORD));
    config.sta.scan_method = WIFI_FAST_SCAN;
    config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    config.sta.bssid_set = true;
    std::memcpy(config.sta.bssid, target.bssid, sizeof(config.sta.bssid));
    config.sta.channel = target.primary;
    config.sta.threshold.rssi = -127;
    config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    config.sta.pmf_cfg.capable = true;
    config.sta.pmf_cfg.required = false;
    return esp_wifi_set_config(WIFI_IF_STA, &config);
}

bool connect_target(int *attempts, uint8_t *last_reason)
{
    if (attempts == nullptr || last_reason == nullptr) {
        return false;
    }
    *attempts = 0;
    *last_reason = 0;
    for (int attempt = 1; attempt <= kConnectAttempts; ++attempt) {
        *attempts = attempt;
        xEventGroupClearBits(s_events, kGotIpBit | kDisconnectedBit);
        s_disconnect_reason.store(0, std::memory_order_relaxed);
        char detail[96] = {};
        std::snprintf(
            detail, sizeof(detail), "Connecting to %s (%d/%d)",
            FACTORY_WIFI_SSID, attempt, kConnectAttempts);
        board_ui_show_status("WI-FI", detail, "Waiting for DHCP (12 s maximum)");
        const esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        const EventBits_t bits = xEventGroupWaitBits(
            s_events, kGotIpBit | kDisconnectedBit,
            pdFALSE, pdFALSE, pdMS_TO_TICKS(kConnectTimeoutMs));
        if ((bits & kGotIpBit) != 0) {
            return true;
        }
        if ((bits & kDisconnectedBit) != 0) {
            *last_reason =
                s_disconnect_reason.load(std::memory_order_relaxed);
        }
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    return false;
}

esp_err_t ping_gateway(const esp_ip4_addr_t &gateway, PingResult *result)
{
    if (result == nullptr || gateway.addr == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *result = {};
    s_ping_replies.store(0, std::memory_order_relaxed);
    s_ping_rtt_sum_ms.store(0, std::memory_order_relaxed);
    s_ping_done = xSemaphoreCreateBinary();
    if (s_ping_done == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    esp_ping_config_t config = ESP_PING_DEFAULT_CONFIG();
    config.count = 3;
    config.interval_ms = 300;
    config.timeout_ms = 1000;
    config.data_size = 32;
    ip_addr_copy_from_ip4(config.target_addr, gateway);
    esp_ping_callbacks_t callbacks = {};
    callbacks.on_ping_success = ping_success;
    callbacks.on_ping_timeout = ping_timeout;
    callbacks.on_ping_end = ping_end;
    ESP_RETURN_ON_ERROR(
        esp_ping_new_session(&config, &callbacks, &s_ping),
        kTag, "create gateway ping");
    ESP_RETURN_ON_ERROR(esp_ping_start(s_ping), kTag, "start gateway ping");
    if (xSemaphoreTake(s_ping_done, pdMS_TO_TICKS(6000)) != pdTRUE) {
        esp_ping_stop(s_ping);
        esp_ping_delete_session(s_ping);
        s_ping = nullptr;
        vSemaphoreDelete(s_ping_done);
        s_ping_done = nullptr;
        return ESP_ERR_TIMEOUT;
    }
    esp_ping_get_profile(
        s_ping, ESP_PING_PROF_REQUEST,
        &result->transmitted, sizeof(result->transmitted));
    esp_ping_get_profile(
        s_ping, ESP_PING_PROF_REPLY,
        &result->received, sizeof(result->received));
    esp_ping_get_profile(
        s_ping, ESP_PING_PROF_DURATION,
        &result->duration_ms, sizeof(result->duration_ms));
    const uint32_t replies = s_ping_replies.load(std::memory_order_relaxed);
    result->average_rtt_ms = replies == 0 ? 0 :
        s_ping_rtt_sum_ms.load(std::memory_order_relaxed) / replies;
    esp_ping_delete_session(s_ping);
    s_ping = nullptr;
    vSemaphoreDelete(s_ping_done);
    s_ping_done = nullptr;
    return ESP_OK;
}

const char *connect_error_code(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return "AUTH_FAILED";
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return "SSID_NOT_FOUND";
    default:
        return "DHCP_TIMEOUT";
    }
}

}  // namespace

esp_err_t wifi_test_run(
    const FactoryTestEnvironment &,
    FactoryTestRecord *record,
    FactoryModuleOutcome *outcome)
{
    if (record == nullptr || outcome == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *outcome = {};
    wifi_test_cleanup();
    if (FACTORY_WIFI_SSID[0] == '\0') {
        set_failure(
            record, "CONFIG_MISSING",
            "Create main/factory_config.h with the factory Wi-Fi credentials");
        return ESP_ERR_INVALID_STATE;
    }

    board_ui_show_status("WI-FI", "Initializing station mode", "Credentials are never included in results");
    esp_err_t err = init_wifi();
    if (err != ESP_OK) {
        set_failure(record, "WIFI_INIT_FAILED", "Unable to initialize Wi-Fi station");
        return err;
    }
    board_ui_show_status("WI-FI", "Scanning factory SSID", FACTORY_WIFI_SSID);
    ScanTarget target = {};
    err = scan_target(&target);
    if (err != ESP_OK) {
        set_failure(record, "SCAN_FAILED", "Wi-Fi scan failed");
        return err;
    }
    if (!target.found) {
        set_failure(record, "SSID_NOT_FOUND", "Factory SSID was not found");
        const std::string ssid = factory_json_escape(FACTORY_WIFI_SSID);
        std::snprintf(
            record->measurements, sizeof(record->measurements),
            "{\"ssid\":\"%s\",\"scan_count\":%u}",
            ssid.c_str(), target.total_count);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_RETURN_ON_ERROR(
        configure_target(target.ap), kTag, "configure factory access point");
    int connect_attempts = 0;
    uint8_t disconnect_reason = 0;
    if (!connect_target(&connect_attempts, &disconnect_reason)) {
        char detail[96] = {};
        std::snprintf(
            detail, sizeof(detail), "Connection failed after %d attempts (reason %u)",
            connect_attempts, disconnect_reason);
        set_failure(record, connect_error_code(disconnect_reason), detail);
        return ESP_ERR_TIMEOUT;
    }

    esp_netif_ip_info_t ip_info = {};
    err = esp_netif_get_ip_info(s_sta_netif, &ip_info);
    if (err != ESP_OK || ip_info.ip.addr == 0 || ip_info.gw.addr == 0) {
        set_failure(record, "DHCP_INVALID", "DHCP did not provide a usable IP and gateway");
        return err == ESP_OK ? ESP_ERR_INVALID_RESPONSE : err;
    }
    char ip[IP4ADDR_STRLEN_MAX] = {};
    char gateway[IP4ADDR_STRLEN_MAX] = {};
    esp_ip4addr_ntoa(&ip_info.ip, ip, sizeof(ip));
    esp_ip4addr_ntoa(&ip_info.gw, gateway, sizeof(gateway));
    char status[96] = {};
    std::snprintf(status, sizeof(status), "IP %s  GW %s", ip, gateway);
    board_ui_show_status("WI-FI", status, "Pinging gateway 3 times");

    PingResult ping = {};
    err = ping_gateway(ip_info.gw, &ping);
    const bool rssi_ok = target.ap.rssi >= FACTORY_WIFI_MIN_RSSI_DBM;
    const bool ping_ok = err == ESP_OK && ping.received >= 2;
    const std::string ssid = factory_json_escape(FACTORY_WIFI_SSID);
    const uint32_t loss_percent = ping.transmitted == 0 ? 100 :
        (ping.transmitted - ping.received) * 100U / ping.transmitted;
    std::snprintf(
        record->measurements, sizeof(record->measurements),
        "{\"ssid\":\"%s\",\"channel\":%u,\"rssi_dbm\":%d,"
        "\"ip\":\"%s\",\"gateway\":\"%s\",\"connect_attempts\":%d,"
        "\"ping_sent\":%lu,\"ping_received\":%lu,\"packet_loss_percent\":%lu,"
        "\"average_rtt_ms\":%lu}",
        ssid.c_str(), target.ap.primary, target.ap.rssi, ip, gateway,
        connect_attempts, static_cast<unsigned long>(ping.transmitted),
        static_cast<unsigned long>(ping.received),
        static_cast<unsigned long>(loss_percent),
        static_cast<unsigned long>(ping.average_rtt_ms));
    if (!rssi_ok) {
        char detail[96] = {};
        std::snprintf(
            detail, sizeof(detail), "RSSI %d dBm is below %d dBm",
            target.ap.rssi, FACTORY_WIFI_MIN_RSSI_DBM);
        set_failure(record, "RSSI_LOW", detail);
        return ESP_FAIL;
    }
    if (!ping_ok) {
        set_failure(
            record, err == ESP_ERR_TIMEOUT ? "PING_TIMEOUT" : "PING_LOSS",
            "Fewer than two gateway ping replies were received");
        return err == ESP_OK ? ESP_FAIL : err;
    }
    outcome->automatic_pass = true;
    std::snprintf(
        record->detail, sizeof(record->detail),
        "Connected at %d dBm; %lu/3 gateway pings received",
        target.ap.rssi, static_cast<unsigned long>(ping.received));
    return ESP_OK;
}

void wifi_test_cleanup()
{
    if (s_ping != nullptr) {
        esp_ping_stop(s_ping);
        esp_ping_delete_session(s_ping);
        s_ping = nullptr;
    }
    if (s_ping_done != nullptr) {
        vSemaphoreDelete(s_ping_done);
        s_ping_done = nullptr;
    }
    if (s_wifi_handler != nullptr) {
        esp_event_handler_instance_unregister(
            WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_handler);
        s_wifi_handler = nullptr;
    }
    if (s_ip_handler != nullptr) {
        esp_event_handler_instance_unregister(
            IP_EVENT, IP_EVENT_STA_GOT_IP, s_ip_handler);
        s_ip_handler = nullptr;
    }
    if (s_wifi_started) {
        esp_wifi_disconnect();
        esp_wifi_stop();
        s_wifi_started = false;
    }
    if (s_wifi_initialized) {
        esp_wifi_deinit();
        s_wifi_initialized = false;
    }
    if (s_sta_netif != nullptr) {
        esp_wifi_clear_default_wifi_driver_and_handlers(s_sta_netif);
        esp_netif_destroy(s_sta_netif);
        s_sta_netif = nullptr;
    }
    if (s_events != nullptr) {
        vEventGroupDelete(s_events);
        s_events = nullptr;
    }
    s_disconnect_reason.store(0, std::memory_order_relaxed);
}
