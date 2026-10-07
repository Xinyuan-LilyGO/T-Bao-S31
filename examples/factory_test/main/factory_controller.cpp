#include "factory_controller.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#include "board_service.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "factory_core.h"
#include "factory_settings.h"
#include "factory_tests.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace {

constexpr char kTag[] = "factory_ctrl";
constexpr char kNvsNamespace[] = "factory_cfg";
constexpr char kNvsCameraKey[] = "camera";
constexpr size_t kActionQueueLength = 24;

enum class ControllerPhase : uint8_t {
    kBoot = 0,
    kRunning,
    kManual,
    kSummary,
};

using ModuleRunFn = esp_err_t (*)(
    const FactoryTestEnvironment &, FactoryTestRecord *, FactoryModuleOutcome *);

struct ModuleDescriptor {
    FactoryTestId id;
    const char *title;
    const char *manual_detail;
    ModuleRunFn run;
    FactoryCleanupFn cleanup;
};

QueueHandle_t s_actions = nullptr;
SemaphoreHandle_t s_print_mutex = nullptr;
FactoryRunResult s_result = {};
FactoryCameraProfile s_camera_profile = FactoryCameraProfile::kUnset;
std::atomic<ControllerPhase> s_phase{ControllerPhase::kBoot};
std::atomic<FactoryTestId> s_active_test{FactoryTestId::kCount};
bool s_camera_locked = false;

esp_err_t run_motor_a(
    const FactoryTestEnvironment &environment,
    FactoryTestRecord *record,
    FactoryModuleOutcome *outcome)
{
    return motor_test_run(
        environment, FactoryTestId::kMotorA, record, outcome);
}

esp_err_t run_motor_b(
    const FactoryTestEnvironment &environment,
    FactoryTestRecord *record,
    FactoryModuleOutcome *outcome)
{
    return motor_test_run(
        environment, FactoryTestId::kMotorB, record, outcome);
}

constexpr std::array<ModuleDescriptor, kFactoryTestCount> kModules = {{
    {FactoryTestId::kDisplayTouch, "DISPLAY / TOUCH",
     "Confirm colors, gray scale, checkerboard and no flicker",
     display_touch_test_run, display_touch_test_cleanup},
    {FactoryTestId::kMicrophone, "MICROPHONE", "", microphone_test_run,
     microphone_test_cleanup},
    {FactoryTestId::kSpeaker, "SPEAKER",
     "Confirm the 1 kHz tone was clean and audible",
     speaker_test_run, speaker_test_cleanup},
    {FactoryTestId::kCamera, "CAMERA", "Confirm the frozen image is clear",
     camera_test_run, camera_test_cleanup},
    {FactoryTestId::kCharger, "CHARGER", "", charger_test_run,
     charger_test_cleanup},
    {FactoryTestId::kMotorA, "MOTOR A",
     "Confirm forward and reverse motion", run_motor_a, motor_test_cleanup},
    {FactoryTestId::kMotorB, "MOTOR B",
     "Confirm forward and reverse motion", run_motor_b, motor_test_cleanup},
    {FactoryTestId::kServo, "SERVO", "Confirm smooth normal movement",
     servo_test_run, servo_test_cleanup},
    {FactoryTestId::kSdCard, "SD CARD", "", sdcard_test_run,
     sdcard_test_cleanup},
    {FactoryTestId::kWifi, "WI-FI", "", wifi_test_run, wifi_test_cleanup},
}};

void print_locked(const char *format, ...)
{
    if (s_print_mutex != nullptr) {
        xSemaphoreTake(s_print_mutex, portMAX_DELAY);
    }
    va_list args;
    va_start(args, format);
    std::vprintf(format, args);
    va_end(args);
    std::fflush(stdout);
    if (s_print_mutex != nullptr) {
        xSemaphoreGive(s_print_mutex);
    }
}

void emit_message(const char *event, const char *message)
{
    const std::string escaped = factory_json_escape(message);
    print_locked(
        "FACTORY_EVENT {\"event\":\"%s\",\"message\":\"%s\"}\n",
        event, escaped.c_str());
}

void emit_test_event(
    const char *event,
    FactoryTestId id,
    FactoryTestStatus status,
    const char *detail = nullptr)
{
    const std::string escaped = factory_json_escape(detail != nullptr ? detail : "");
    print_locked(
        "FACTORY_EVENT {\"event\":\"%s\",\"id\":\"%s\","
        "\"status\":\"%s\",\"detail\":\"%s\"}\n",
        event, factory_test_id_name(id), factory_test_status_name(status),
        escaped.c_str());
}

void emit_status()
{
    const std::string json = factory_result_json(s_result);
    print_locked("FACTORY_EVENT {\"event\":\"status\",\"run\":%s}\n", json.c_str());
}

void emit_result()
{
    const std::string json = factory_result_json(s_result);
    print_locked("FACTORY_RESULT %s\n", json.c_str());
}

esp_err_t init_nvs()
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), kTag, "erase incompatible NVS");
        err = nvs_flash_init();
    }
    return err;
}

FactoryCameraProfile load_camera_profile()
{
    nvs_handle_t handle = 0;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) {
        return FactoryCameraProfile::kUnset;
    }
    uint8_t value = 0;
    const esp_err_t err = nvs_get_u8(handle, kNvsCameraKey, &value);
    nvs_close(handle);
    if (err != ESP_OK ||
        (value != static_cast<uint8_t>(FactoryCameraProfile::kOv2640) &&
         value != static_cast<uint8_t>(FactoryCameraProfile::kOv3660))) {
        return FactoryCameraProfile::kUnset;
    }
    return static_cast<FactoryCameraProfile>(value);
}

esp_err_t save_camera_profile(FactoryCameraProfile profile)
{
    nvs_handle_t handle = 0;
    ESP_RETURN_ON_ERROR(
        nvs_open(kNvsNamespace, NVS_READWRITE, &handle),
        kTag, "open factory camera configuration");
    esp_err_t err = nvs_set_u8(
        handle, kNvsCameraKey, static_cast<uint8_t>(profile));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

bool action_allowed_now(const FactoryAction &action)
{
    const ControllerPhase phase = s_phase.load(std::memory_order_relaxed);
    const FactoryTestId active = s_active_test.load(std::memory_order_relaxed);
    switch (action.type) {
    case FactoryActionType::kStatus:
        return true;
    case FactoryActionType::kRun:
    case FactoryActionType::kCameraOv2640:
    case FactoryActionType::kCameraOv3660:
        return phase == ControllerPhase::kBoot;
    case FactoryActionType::kManualPass:
    case FactoryActionType::kManualFail:
        return phase == ControllerPhase::kManual && action.test_id == active;
    case FactoryActionType::kRetry:
        return (phase == ControllerPhase::kManual && action.test_id == active) ||
               phase == ControllerPhase::kSummary;
    case FactoryActionType::kRetestAll:
        return phase == ControllerPhase::kSummary;
    case FactoryActionType::kTouchHit:
        return phase == ControllerPhase::kRunning &&
               active == FactoryTestId::kDisplayTouch;
    default:
        return false;
    }
}

void post_action(const FactoryAction &action)
{
    if (s_actions == nullptr || !action_allowed_now(action)) {
        emit_message("command_rejected", "Command is not valid in the current state");
        return;
    }
    if (xQueueSend(s_actions, &action, 0) != pdTRUE) {
        emit_message("queue_full", "Action queue is full");
    }
}

void ui_action_callback(const FactoryAction &action, void *)
{
    post_action(action);
}

void uart_reader_task(void *)
{
    std::setvbuf(stdin, nullptr, _IONBF, 0);
    char line[128] = {};
    while (true) {
        if (std::fgets(line, sizeof(line), stdin) == nullptr) {
            clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        FactoryAction action = {};
        char error[48] = {};
        if (!factory_parse_command(line, &action, error, sizeof(error))) {
            emit_message("command_error", error);
            continue;
        }
        post_action(action);
    }
}

bool receive_action(FactoryAction *action, uint32_t timeout_ms)
{
    if (action == nullptr || s_actions == nullptr) {
        return false;
    }
    return xQueueReceive(
               s_actions, action,
               timeout_ms == UINT32_MAX ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms)) ==
           pdTRUE;
}

bool process_common_action(const FactoryAction &action)
{
    if (action.type == FactoryActionType::kStatus) {
        emit_status();
        return true;
    }
    if (action.type == FactoryActionType::kCameraOv2640 ||
        action.type == FactoryActionType::kCameraOv3660) {
        if (s_camera_locked) {
            emit_message("camera_locked", "Camera profile is locked after RUN");
            return true;
        }
        const FactoryCameraProfile profile =
            action.type == FactoryActionType::kCameraOv2640 ?
                FactoryCameraProfile::kOv2640 : FactoryCameraProfile::kOv3660;
        s_camera_profile = profile;
        s_result.camera_config = profile;
        const esp_err_t err = save_camera_profile(profile);
        char message[96] = {};
        std::snprintf(
            message, sizeof(message), "Camera profile %s%s",
            factory_camera_profile_name(profile),
            err == ESP_OK ? " saved" : " selected; NVS save failed");
        emit_message(err == ESP_OK ? "camera_config" : "camera_config_error", message);
        board_ui_show_boot(s_result.firmware_version, s_camera_profile, message);
        return true;
    }
    return false;
}

bool wait_for_module_action(
    FactoryAction *action, uint32_t timeout_ms, void *)
{
    const int64_t deadline = esp_timer_get_time() +
        static_cast<int64_t>(timeout_ms) * 1000;
    while (esp_timer_get_time() < deadline) {
        const uint32_t remaining_ms = static_cast<uint32_t>(std::max<int64_t>(
            1, (deadline - esp_timer_get_time()) / 1000));
        FactoryAction received = {};
        if (!receive_action(&received, remaining_ms)) {
            return false;
        }
        if (process_common_action(received)) {
            continue;
        }
        *action = received;
        return true;
    }
    return false;
}

void initialize_result_metadata()
{
    s_result = {};
    s_result.schema = FACTORY_RESULT_SCHEMA;
    std::snprintf(
        s_result.device_name, sizeof(s_result.device_name), "%s",
        FACTORY_DEVICE_NAME);
    const esp_app_desc_t *description = esp_app_get_description();
    std::snprintf(
        s_result.firmware_version, sizeof(s_result.firmware_version), "%s",
        description != nullptr ? description->version : "unknown");
    std::snprintf(
        s_result.idf_version, sizeof(s_result.idf_version), "%s",
        esp_get_idf_version());
    uint8_t mac[6] = {};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        std::snprintf(
            s_result.sta_mac, sizeof(s_result.sta_mac),
            "%02X:%02X:%02X:%02X:%02X:%02X",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        std::snprintf(s_result.sta_mac, sizeof(s_result.sta_mac), "UNKNOWN");
    }
    factory_reset_run(&s_result);
}

void set_status(FactoryTestRecord *record, FactoryTestStatus status)
{
    if (record == nullptr) {
        return;
    }
    if (!factory_status_transition_valid(record->status, status)) {
        ESP_LOGW(
            kTag, "Invalid status transition %s -> %s for %s",
            factory_test_status_name(record->status),
            factory_test_status_name(status),
            factory_test_id_name(record->id));
    }
    record->status = status;
    emit_test_event("test_status", record->id, status, record->detail);
}

void prepare_attempt(FactoryTestRecord *record)
{
    const FactoryTestId id = record->id;
    const uint16_t attempts = static_cast<uint16_t>(record->attempts + 1);
    const uint32_t duration_ms = record->duration_ms;
    *record = {};
    record->id = id;
    record->attempts = attempts;
    record->duration_ms = duration_ms;
    std::snprintf(record->measurements, sizeof(record->measurements), "{}");
}

void set_failure_if_empty(
    FactoryTestRecord *record, const char *code, const char *detail)
{
    if (record->error_code[0] == '\0') {
        std::snprintf(record->error_code, sizeof(record->error_code), "%s", code);
    }
    if (record->detail[0] == '\0') {
        std::snprintf(record->detail, sizeof(record->detail), "%s", detail);
    }
}

FactoryManualDecision wait_manual(const ModuleDescriptor &module)
{
    s_phase.store(ControllerPhase::kManual, std::memory_order_relaxed);
    if (module.id != FactoryTestId::kCamera) {
        board_ui_show_manual(
            module.id, module.title, module.manual_detail, true);
    }
    const int64_t deadline = esp_timer_get_time() +
        static_cast<int64_t>(FACTORY_MANUAL_TIMEOUT_MS) * 1000;
    while (esp_timer_get_time() < deadline) {
        const uint32_t remaining_ms = static_cast<uint32_t>(std::max<int64_t>(
            1, (deadline - esp_timer_get_time()) / 1000));
        FactoryAction action = {};
        if (!receive_action(&action, remaining_ms)) {
            break;
        }
        if (process_common_action(action)) {
            continue;
        }
        const FactoryManualDecision decision =
            factory_manual_decision(module.id, action, false);
        if (decision != FactoryManualDecision::kIgnore) {
            return decision;
        }
    }
    return factory_manual_decision(module.id, {}, true);
}

bool execute_module(const ModuleDescriptor &module)
{
    FactoryTestRecord &record =
        s_result.tests[static_cast<size_t>(module.id)];
    bool retry = false;
    bool safety_ok = true;
    do {
        retry = false;
        prepare_attempt(&record);
        s_active_test.store(module.id, std::memory_order_relaxed);
        s_phase.store(ControllerPhase::kRunning, std::memory_order_relaxed);
        set_status(&record, FactoryTestStatus::kRunning);
        ESP_LOGI(
            kTag, "Controller stack minimum free before %s: %lu bytes",
            factory_test_id_name(module.id),
            static_cast<unsigned long>(uxTaskGetStackHighWaterMark(nullptr)));
        board_ui_show_status(module.title, "Running automatic checks", factory_test_id_name(module.id));
        const int64_t attempt_start_us = esp_timer_get_time();

        FactoryTestEnvironment environment = {};
        environment.camera_profile = s_camera_profile;
        environment.run_result = &s_result;
        environment.wait_action = wait_for_module_action;
        FactoryModuleOutcome outcome = {};
        esp_err_t run_error = ESP_FAIL;
        {
            FactoryCleanupGuard cleanup(module.cleanup);
            run_error = module.run(environment, &record, &outcome);
            const bool automatic_pass =
                run_error == ESP_OK && outcome.automatic_pass;
            if (outcome.manual_required && automatic_pass) {
                set_status(&record, FactoryTestStatus::kWaitingManual);
                const FactoryManualDecision decision = wait_manual(module);
                if (decision == FactoryManualDecision::kRetry &&
                    outcome.replay_supported) {
                    retry = true;
                } else if (decision == FactoryManualDecision::kFail) {
                    set_failure_if_empty(
                        &record, "MANUAL_REJECT", "Operator rejected the test");
                } else if (decision == FactoryManualDecision::kTimeout) {
                    set_failure_if_empty(
                        &record, "TIMEOUT", "Operator confirmation timed out");
                } else if (decision != FactoryManualDecision::kPass) {
                    set_failure_if_empty(
                        &record, "INVALID_ACTION", "Manual confirmation was invalid");
                }
                if (!retry && automatic_pass &&
                    decision == FactoryManualDecision::kPass) {
                    record.error_code[0] = '\0';
                }
            } else if (!automatic_pass) {
                set_failure_if_empty(
                    &record, "CHECK_FAILED", "Automatic checks did not pass");
            }
            cleanup.run_now();
            if (board_service_state().expander_ready) {
                const esp_err_t safe_error = board_service_apply_safe_outputs();
                if (safe_error != ESP_OK) {
                    retry = false;
                    safety_ok = false;
                    std::snprintf(
                        record.error_code, sizeof(record.error_code),
                        "CLEANUP_FAILED");
                    std::snprintf(
                        record.detail, sizeof(record.detail),
                        "Unable to restore XL9555 safe outputs");
                    run_error = safe_error;
                    outcome.automatic_pass = false;
                }
            }
        }

        ESP_LOGI(
            kTag, "Controller stack minimum free after %s: %lu bytes",
            factory_test_id_name(module.id),
            static_cast<unsigned long>(uxTaskGetStackHighWaterMark(nullptr)));

        record.duration_ms += static_cast<uint32_t>(
            (esp_timer_get_time() - attempt_start_us) / 1000);
        if (retry) {
            emit_test_event("test_retry", module.id, record.status, record.detail);
            continue;
        }
        const bool passed = run_error == ESP_OK && outcome.automatic_pass &&
            (!outcome.manual_required || record.error_code[0] == '\0');
        set_status(
            &record,
            passed ? FactoryTestStatus::kPass : FactoryTestStatus::kFail);
    } while (retry);
    s_active_test.store(FactoryTestId::kCount, std::memory_order_relaxed);
    return safety_ok;
}

void mark_safety_aborted_tests(size_t first_index, FactoryTestId failed_after)
{
    char detail[128] = {};
    std::snprintf(
        detail, sizeof(detail),
        "Run aborted after %s cleanup could not restore XL9555 safe outputs",
        factory_test_id_name(failed_after));
    emit_message("safe_output_error", detail);
    board_ui_show_status("SAFETY STOP", detail, "Remaining tests were not started");
    for (size_t index = first_index; index < s_result.tests.size(); ++index) {
        FactoryTestRecord &record = s_result.tests[index];
        record.status = FactoryTestStatus::kFail;
        std::snprintf(
            record.error_code, sizeof(record.error_code),
            "SAFE_OUTPUT_FAILED");
        std::snprintf(record.detail, sizeof(record.detail), "%s", detail);
        emit_test_event(
            "test_status", record.id, record.status, record.detail);
    }
}

uint32_t total_test_duration_ms()
{
    uint64_t total = 0;
    for (const FactoryTestRecord &record : s_result.tests) {
        total += record.duration_ms;
    }
    return static_cast<uint32_t>(std::min<uint64_t>(total, UINT32_MAX));
}

void finalize_run()
{
    factory_aggregate_pass(&s_result);
    s_result.duration_ms = total_test_duration_ms();
    s_phase.store(ControllerPhase::kSummary, std::memory_order_relaxed);
    s_active_test.store(FactoryTestId::kCount, std::memory_order_relaxed);
    board_ui_show_summary(s_result);
    emit_result();
}

void create_run_id()
{
    char compact_mac[13] = {};
    unsigned bytes[6] = {};
    if (std::sscanf(
            s_result.sta_mac, "%02X:%02X:%02X:%02X:%02X:%02X",
            &bytes[0], &bytes[1], &bytes[2], &bytes[3], &bytes[4], &bytes[5]) == 6) {
        std::snprintf(
            compact_mac, sizeof(compact_mac), "%02X%02X%02X%02X%02X%02X",
            bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5]);
    } else {
        std::snprintf(compact_mac, sizeof(compact_mac), "UNKNOWN");
    }
    std::snprintf(
        s_result.run_id, sizeof(s_result.run_id), "%s-%llu",
        compact_mac,
        static_cast<unsigned long long>(esp_timer_get_time() / 1000));
}

void run_all_tests()
{
    factory_reset_run(&s_result);
    s_result.camera_config = s_camera_profile;
    create_run_id();
    s_camera_locked = true;
    emit_message("run_start", s_result.run_id);
    const bool safe_outputs_ready =
        board_service_state().expander_ready &&
        board_service_apply_safe_outputs() == ESP_OK;
    if (!safe_outputs_ready) {
        const char *detail = "Unable to establish XL9555 safe outputs";
        emit_message("safe_output_error", detail);
        board_ui_show_status("SAFETY STOP", detail, "No hardware tests were started");
        for (FactoryTestRecord &record : s_result.tests) {
            record.status = FactoryTestStatus::kFail;
            std::snprintf(
                record.error_code, sizeof(record.error_code),
                "SAFE_OUTPUT_FAILED");
            std::snprintf(record.detail, sizeof(record.detail), "%s", detail);
            emit_test_event(
                "test_status", record.id, record.status, record.detail);
        }
        finalize_run();
        return;
    }
    for (size_t index = 0; index < kModules.size(); ++index) {
        const ModuleDescriptor &module = kModules[index];
        if (!execute_module(module)) {
            mark_safety_aborted_tests(index + 1, module.id);
            break;
        }
    }
    finalize_run();
}

void retry_test(FactoryTestId id)
{
    const size_t index = static_cast<size_t>(id);
    if (index >= kModules.size() ||
        s_result.tests[index].status != FactoryTestStatus::kFail) {
        emit_message("retry_rejected", "Only failed tests can be retried from summary");
        return;
    }
    if (!board_service_state().expander_ready ||
        board_service_apply_safe_outputs() != ESP_OK) {
        FactoryTestRecord &record = s_result.tests[index];
        std::snprintf(
            record.error_code, sizeof(record.error_code),
            "SAFE_OUTPUT_FAILED");
        std::snprintf(
            record.detail, sizeof(record.detail),
            "Retry blocked because XL9555 safe outputs could not be restored");
        emit_message("safe_output_error", record.detail);
        emit_test_event(
            "test_status", record.id, record.status, record.detail);
        finalize_run();
        return;
    }
    execute_module(kModules[index]);
    finalize_run();
}

void boot_loop()
{
    s_phase.store(ControllerPhase::kBoot, std::memory_order_relaxed);
    while (true) {
        FactoryAction action = {};
        if (!receive_action(&action, UINT32_MAX)) {
            continue;
        }
        if (process_common_action(action)) {
            continue;
        }
        if (action.type != FactoryActionType::kRun) {
            continue;
        }
        if (s_camera_profile == FactoryCameraProfile::kUnset) {
            const char *message = "Select OV2640 or OV3660 before RUN";
            emit_message("run_rejected", message);
            board_ui_show_boot(s_result.firmware_version, s_camera_profile, message);
            continue;
        }
        run_all_tests();
        return;
    }
}

void summary_loop()
{
    while (true) {
        FactoryAction action = {};
        if (!receive_action(&action, UINT32_MAX)) {
            continue;
        }
        if (process_common_action(action)) {
            continue;
        }
        if (action.type == FactoryActionType::kRetestAll) {
            run_all_tests();
        } else if (action.type == FactoryActionType::kRetry) {
            retry_test(action.test_id);
        }
    }
}

}  // namespace

void factory_controller_run()
{
    s_print_mutex = xSemaphoreCreateMutex();
    s_actions = xQueueCreate(kActionQueueLength, sizeof(FactoryAction));
    if (s_print_mutex == nullptr || s_actions == nullptr) {
        ESP_LOGE(kTag, "Unable to allocate controller resources");
        return;
    }
    if (init_nvs() != ESP_OK) {
        emit_message("nvs_error", "NVS initialization failed; camera selection will not persist");
    }
    initialize_result_metadata();
    s_camera_profile = load_camera_profile();
    s_result.camera_config = s_camera_profile;

    const esp_err_t board_error = board_service_init(ui_action_callback, nullptr);
    board_ui_show_boot(
        s_result.firmware_version, s_camera_profile,
        s_camera_profile == FactoryCameraProfile::kUnset ?
            "Select camera profile before RUN" : nullptr);
    if (board_service_enable_backlight() != ESP_OK) {
        emit_message("display_unavailable", "Use the serial command interface");
    }
    if (board_error != ESP_OK) {
        emit_message("board_init_partial", esp_err_to_name(board_error));
    }
    if (xTaskCreate(
            uart_reader_task, "factory_uart", 4096, nullptr, 5, nullptr) != pdPASS) {
        emit_message("uart_error", "Unable to start serial command reader");
    }

    print_locked(
        "FACTORY_EVENT {\"event\":\"ready\",\"device\":\"%s\","
        "\"firmware\":\"%s\",\"camera\":\"%s\"}\n",
        s_result.device_name, s_result.firmware_version,
        factory_camera_profile_name(s_camera_profile));
    boot_loop();
    summary_loop();
}
