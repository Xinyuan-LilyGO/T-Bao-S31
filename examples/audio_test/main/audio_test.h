#pragma once

#include <cstdint>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_io_expander.h"

enum class TestResult : uint8_t {
    kPending,
    kPass,
    kFail,
    kManual,
};

enum class TestMode : uint8_t {
    kIdle,
    kMic,
    kDac,
    kAuto,
};

struct AudioTestState {
    TestResult es8389 = TestResult::kPending;
    TestResult es7210 = TestResult::kPending;
    TestResult i2s_rx = TestResult::kPending;
    TestResult i2s_tx = TestResult::kPending;
    TestResult dac_sound = TestResult::kPending;
    TestMode mode = TestMode::kIdle;
    float rms_dbfs = -96.0f;
    float peak_dbfs = -96.0f;
    uint8_t level_percent = 0;
    char detail[64] = "INITIALIZING";
};

using AudioStateCallback = void (*)(const AudioTestState &state, void *context);

esp_err_t audio_test_start(
    i2c_master_bus_handle_t i2c_bus,
    esp_io_expander_handle_t io_expander,
    AudioStateCallback callback,
    void *callback_context);

void audio_test_request(TestMode mode);
