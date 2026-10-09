#pragma once

#include <cstdint>

#include "esp_err.h"
#include "factory_core.h"

using FactoryWaitActionFn =
    bool (*)(FactoryAction *action, uint32_t timeout_ms, void *context);

struct FactoryTestEnvironment {
    FactoryCameraProfile camera_profile = FactoryCameraProfile::kUnset;
    FactoryRunResult *run_result = nullptr;
    FactoryWaitActionFn wait_action = nullptr;
    void *wait_context = nullptr;
};

struct FactoryModuleOutcome {
    bool automatic_pass = false;
    bool manual_required = false;
    bool replay_supported = false;
};

esp_err_t display_touch_test_run(
    const FactoryTestEnvironment &, FactoryTestRecord *, FactoryModuleOutcome *);
void display_touch_test_cleanup();
esp_err_t touch_button_test_run(
    const FactoryTestEnvironment &, FactoryTestRecord *, FactoryModuleOutcome *);
void touch_button_test_cleanup();
esp_err_t gpio60_button_test_run(
    const FactoryTestEnvironment &, FactoryTestRecord *, FactoryModuleOutcome *);
void gpio60_button_test_cleanup();
esp_err_t boot_button_test_run(
    const FactoryTestEnvironment &, FactoryTestRecord *, FactoryModuleOutcome *);
void boot_button_test_cleanup();
esp_err_t microphone_test_run(
    const FactoryTestEnvironment &, FactoryTestRecord *, FactoryModuleOutcome *);
void microphone_test_cleanup();
esp_err_t speaker_test_run(
    const FactoryTestEnvironment &, FactoryTestRecord *, FactoryModuleOutcome *);
void speaker_test_cleanup();
esp_err_t camera_test_run(
    const FactoryTestEnvironment &, FactoryTestRecord *, FactoryModuleOutcome *);
void camera_test_cleanup();
esp_err_t charger_test_run(
    const FactoryTestEnvironment &, FactoryTestRecord *, FactoryModuleOutcome *);
void charger_test_cleanup();
esp_err_t motor_test_run(
    const FactoryTestEnvironment &, FactoryTestId, FactoryTestRecord *, FactoryModuleOutcome *);
esp_err_t motor_outputs_init_safe();
void motor_test_cleanup();
esp_err_t servo_test_run(
    const FactoryTestEnvironment &, FactoryTestRecord *, FactoryModuleOutcome *);
void servo_test_cleanup();
esp_err_t sdcard_test_run(
    const FactoryTestEnvironment &, FactoryTestRecord *, FactoryModuleOutcome *);
void sdcard_test_cleanup();
esp_err_t wifi_test_run(
    const FactoryTestEnvironment &, FactoryTestRecord *, FactoryModuleOutcome *);
void wifi_test_cleanup();
