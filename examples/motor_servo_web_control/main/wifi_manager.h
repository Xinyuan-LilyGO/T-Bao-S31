#pragma once

#include <cstdint>

#include "esp_err.h"

enum class WifiPhase {
    kInitializing,
    kScanning,
    kTargetNotConfigured,
    kTargetNotFound,
    kConnecting,
    kConnected,
    kConnectionFailed,
    kDisconnected,
};

struct WifiStatus {
    WifiPhase phase = WifiPhase::kInitializing;
    uint16_t ap_count = 0;
    bool target_found = false;
    int target_rssi = -127;
    char detail[72] = {};
    char ip_address[16] = {};
    char web_url[32] = {};
};

using WifiStatusCallback = void (*)(const WifiStatus &status, void *context);

esp_err_t wifi_manager_start(WifiStatusCallback callback, void *context);
