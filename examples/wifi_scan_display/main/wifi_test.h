#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

constexpr size_t kWifiDisplayApLimit = 8;

enum class WifiTestPhase {
    kInitializing,
    kScanning,
    kTargetNotConfigured,
    kTargetNotFound,
    kConnecting,
    kConnected,
    kConnectionFailed,
    kDisconnected,
};

struct WifiAccessPoint {
    char ssid[33] = {};
    int8_t rssi = -127;
    uint8_t channel = 0;
    bool secured = false;
    bool is_target = false;
};

struct WifiTestState {
    WifiTestPhase phase = WifiTestPhase::kInitializing;
    uint16_t total_ap_count = 0;
    size_t visible_ap_count = 0;
    WifiAccessPoint access_points[kWifiDisplayApLimit] = {};
    char detail[64] = {};
};

using WifiStateCallback = void (*)(const WifiTestState &state, void *context);

esp_err_t wifi_test_start(WifiStateCallback callback, void *context);
