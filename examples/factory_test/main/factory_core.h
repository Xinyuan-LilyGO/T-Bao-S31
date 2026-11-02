#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

enum class FactoryTestId : uint8_t {
    kDisplayTouch = 0,
    kTouchButton,
    kGpio60Button,
    kBootButton,
    kMicrophone,
    kSpeaker,
    kCamera,
    kCharger,
    kMotorA,
    kMotorB,
    kServo,
    kSdCard,
    kWifi,
    kCount,
};

enum class FactoryTestStatus : uint8_t {
    kPending = 0,
    kRunning,
    kWaitingManual,
    kPass,
    kFail,
};

enum class FactoryCameraProfile : uint8_t {
    kUnset = 0,
    kOv2640,
    kOv3660,
};

enum class FactoryActionType : uint8_t {
    kNone = 0,
    kRun,
    kCameraOv2640,
    kCameraOv3660,
    kManualPass,
    kManualFail,
    kRetry,
    kRetestAll,
    kStatus,
    kTouchHit,
};

enum class FactoryManualDecision : uint8_t {
    kIgnore = 0,
    kPass,
    kFail,
    kRetry,
    kTimeout,
};

struct FactoryAction {
    FactoryActionType type = FactoryActionType::kNone;
    FactoryTestId test_id = FactoryTestId::kCount;
    uint8_t value = 0;
};

struct FactoryTestRecord {
    FactoryTestId id = FactoryTestId::kDisplayTouch;
    FactoryTestStatus status = FactoryTestStatus::kPending;
    uint32_t duration_ms = 0;
    uint16_t attempts = 0;
    char error_code[32] = {};
    char detail[128] = {};
    char measurements[384] = "{}";
};

constexpr size_t kFactoryTestCount =
    static_cast<size_t>(FactoryTestId::kCount);

struct FactoryRunResult {
    uint32_t schema = 1;
    char run_id[48] = {};
    char device_name[24] = {};
    char firmware_version[32] = {};
    char idf_version[32] = {};
    char sta_mac[24] = {};
    FactoryCameraProfile camera_config = FactoryCameraProfile::kUnset;
    char camera_detected[20] = "UNKNOWN";
    uint8_t camera_address = 0;
    uint16_t camera_pid = 0;
    bool overall_pass = false;
    uint32_t duration_ms = 0;
    std::array<FactoryTestRecord, kFactoryTestCount> tests = {};
};

using FactoryCleanupFn = void (*)();

class FactoryCleanupGuard {
public:
    explicit FactoryCleanupGuard(FactoryCleanupFn cleanup) : cleanup_(cleanup) {}
    ~FactoryCleanupGuard();

    FactoryCleanupGuard(const FactoryCleanupGuard &) = delete;
    FactoryCleanupGuard &operator=(const FactoryCleanupGuard &) = delete;

    void run_now();
    void dismiss();

private:
    FactoryCleanupFn cleanup_ = nullptr;
};

const char *factory_test_id_name(FactoryTestId id);
const char *factory_test_status_name(FactoryTestStatus status);
const char *factory_camera_profile_name(FactoryCameraProfile profile);
bool factory_parse_test_id(const char *text, FactoryTestId *id);
bool factory_parse_command(
    const char *line,
    FactoryAction *action,
    char *error,
    size_t error_size);
bool factory_status_transition_valid(
    FactoryTestStatus from,
    FactoryTestStatus to);
FactoryManualDecision factory_manual_decision(
    FactoryTestId waiting_test,
    const FactoryAction &action,
    bool timed_out);
bool factory_mic_threshold_pass(
    float left_dbfs,
    float right_dbfs,
    float threshold_dbfs,
    bool left_nonzero,
    bool right_nonzero,
    bool sustained_clipping);
bool factory_charger_threshold_pass(
    int vbus_mv,
    int vbat_mv,
    int ichg_ma,
    bool charge_done,
    bool active_charge,
    bool ntc_ok,
    uint8_t fault_bits);
void factory_reset_run(FactoryRunResult *result);
bool factory_aggregate_pass(FactoryRunResult *result);
std::string factory_json_escape(const char *text);
std::string factory_result_json(const FactoryRunResult &result);
