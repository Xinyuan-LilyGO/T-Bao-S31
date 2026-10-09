#include "factory_core.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <sstream>

#include "factory_settings.h"

namespace {

constexpr const char *kTestNames[] = {
    "display_touch", "touch_button", "io60_button", "boot_button",
    "microphone", "speaker", "camera", "charger",
    "motor_a", "motor_b", "servo", "sd_card", "wifi",
};
static_assert(sizeof(kTestNames) / sizeof(kTestNames[0]) == kFactoryTestCount);

bool equals_ignore_case(const char *left, const char *right)
{
    if (left == nullptr || right == nullptr) {
        return false;
    }
    while (*left != '\0' && *right != '\0') {
        if (std::tolower(static_cast<unsigned char>(*left)) !=
            std::tolower(static_cast<unsigned char>(*right))) {
            return false;
        }
        ++left;
        ++right;
    }
    return *left == '\0' && *right == '\0';
}

void set_parse_error(char *error, size_t error_size, const char *text)
{
    if (error != nullptr && error_size > 0) {
        std::snprintf(error, error_size, "%s", text);
    }
}

}  // namespace

FactoryCleanupGuard::~FactoryCleanupGuard()
{
    run_now();
}

void FactoryCleanupGuard::run_now()
{
    if (cleanup_ != nullptr) {
        FactoryCleanupFn cleanup = cleanup_;
        cleanup_ = nullptr;
        cleanup();
    }
}

void FactoryCleanupGuard::dismiss()
{
    cleanup_ = nullptr;
}

const char *factory_test_id_name(FactoryTestId id)
{
    const size_t index = static_cast<size_t>(id);
    return index < kFactoryTestCount ? kTestNames[index] : "unknown";
}

const char *factory_test_status_name(FactoryTestStatus status)
{
    switch (status) {
    case FactoryTestStatus::kPending:
        return "PENDING";
    case FactoryTestStatus::kRunning:
        return "RUNNING";
    case FactoryTestStatus::kWaitingManual:
        return "WAITING_MANUAL";
    case FactoryTestStatus::kPass:
        return "PASS";
    case FactoryTestStatus::kFail:
        return "FAIL";
    default:
        return "UNKNOWN";
    }
}

const char *factory_camera_profile_name(FactoryCameraProfile profile)
{
    switch (profile) {
    case FactoryCameraProfile::kOv2640:
        return "OV2640";
    case FactoryCameraProfile::kOv3660:
        return "OV3660";
    case FactoryCameraProfile::kUnset:
    default:
        return "UNSET";
    }
}

bool factory_parse_test_id(const char *text, FactoryTestId *id)
{
    if (text == nullptr || id == nullptr) {
        return false;
    }
    for (size_t i = 0; i < kFactoryTestCount; ++i) {
        if (equals_ignore_case(text, kTestNames[i])) {
            *id = static_cast<FactoryTestId>(i);
            return true;
        }
    }
    return false;
}

bool factory_parse_command(
    const char *line, FactoryAction *action, char *error, size_t error_size)
{
    if (line == nullptr || action == nullptr) {
        set_parse_error(error, error_size, "INVALID_ARGUMENT");
        return false;
    }
    std::istringstream input(line);
    std::string command;
    std::string argument;
    std::string extra;
    if (!(input >> command)) {
        set_parse_error(error, error_size, "EMPTY_COMMAND");
        return false;
    }
    const bool has_argument = static_cast<bool>(input >> argument);
    const bool has_extra = static_cast<bool>(input >> extra);

    *action = FactoryAction{};
    if (equals_ignore_case(command.c_str(), "RUN") && !has_argument) {
        action->type = FactoryActionType::kRun;
        return true;
    }
    if (equals_ignore_case(command.c_str(), "STATUS") && !has_argument) {
        action->type = FactoryActionType::kStatus;
        return true;
    }
    if (equals_ignore_case(command.c_str(), "RETEST") && has_argument &&
        equals_ignore_case(argument.c_str(), "ALL") && !has_extra) {
        action->type = FactoryActionType::kRetestAll;
        return true;
    }
    if (equals_ignore_case(command.c_str(), "CAMERA") && has_argument &&
        !has_extra) {
        if (equals_ignore_case(argument.c_str(), "OV2640")) {
            action->type = FactoryActionType::kCameraOv2640;
            return true;
        }
        if (equals_ignore_case(argument.c_str(), "OV3660")) {
            action->type = FactoryActionType::kCameraOv3660;
            return true;
        }
        set_parse_error(error, error_size, "BAD_CAMERA_PROFILE");
        return false;
    }

    FactoryActionType type = FactoryActionType::kNone;
    if (equals_ignore_case(command.c_str(), "PASS")) {
        type = FactoryActionType::kManualPass;
    } else if (equals_ignore_case(command.c_str(), "FAIL")) {
        type = FactoryActionType::kManualFail;
    } else if (equals_ignore_case(command.c_str(), "RETRY")) {
        type = FactoryActionType::kRetry;
    }
    if (type != FactoryActionType::kNone && has_argument && !has_extra) {
        FactoryTestId id = FactoryTestId::kCount;
        if (!factory_parse_test_id(argument.c_str(), &id)) {
            set_parse_error(error, error_size, "BAD_TEST_ID");
            return false;
        }
        action->type = type;
        action->test_id = id;
        return true;
    }
    set_parse_error(error, error_size, "BAD_COMMAND");
    return false;
}

bool factory_status_transition_valid(FactoryTestStatus from, FactoryTestStatus to)
{
    if (to == FactoryTestStatus::kPending) {
        return true;
    }
    switch (from) {
    case FactoryTestStatus::kPending:
    case FactoryTestStatus::kPass:
    case FactoryTestStatus::kFail:
        return to == FactoryTestStatus::kRunning;
    case FactoryTestStatus::kRunning:
        return to == FactoryTestStatus::kWaitingManual ||
               to == FactoryTestStatus::kPass || to == FactoryTestStatus::kFail;
    case FactoryTestStatus::kWaitingManual:
        return to == FactoryTestStatus::kRunning ||
               to == FactoryTestStatus::kPass || to == FactoryTestStatus::kFail;
    default:
        return false;
    }
}

FactoryManualDecision factory_manual_decision(
    FactoryTestId waiting_test,
    const FactoryAction &action,
    bool timed_out)
{
    if (timed_out) {
        return FactoryManualDecision::kTimeout;
    }
    if (action.test_id != waiting_test) {
        return FactoryManualDecision::kIgnore;
    }
    switch (action.type) {
    case FactoryActionType::kManualPass:
        return FactoryManualDecision::kPass;
    case FactoryActionType::kManualFail:
        return FactoryManualDecision::kFail;
    case FactoryActionType::kRetry:
        return FactoryManualDecision::kRetry;
    default:
        return FactoryManualDecision::kIgnore;
    }
}

bool factory_mic_threshold_pass(
    float left_dbfs, float right_dbfs, float threshold_dbfs,
    bool left_nonzero, bool right_nonzero, bool sustained_clipping)
{
    return left_nonzero && right_nonzero && !sustained_clipping &&
           left_dbfs >= threshold_dbfs && right_dbfs >= threshold_dbfs;
}

bool factory_charger_threshold_pass(
    int vbus_mv, int vbat_mv, int ichg_ma, bool charge_done,
    bool active_charge, bool ntc_ok, uint8_t fault_bits)
{
    const bool voltage_ok =
        vbus_mv >= FACTORY_CHARGER_VBUS_MIN_MV &&
        vbat_mv >= FACTORY_CHARGER_VBAT_MIN_MV &&
        vbat_mv <= FACTORY_CHARGER_VBAT_MAX_MV;
    const bool current_ok = charge_done || (active_charge && ichg_ma > 0);
    return voltage_ok && current_ok && ntc_ok && fault_bits == 0;
}

void factory_reset_run(FactoryRunResult *result)
{
    if (result == nullptr) {
        return;
    }
    result->overall_pass = false;
    result->duration_ms = 0;
    std::snprintf(result->camera_detected, sizeof(result->camera_detected), "UNKNOWN");
    result->camera_address = 0;
    result->camera_pid = 0;
    for (size_t i = 0; i < result->tests.size(); ++i) {
        FactoryTestRecord &record = result->tests[i];
        record = FactoryTestRecord{};
        record.id = static_cast<FactoryTestId>(i);
        std::snprintf(record.measurements, sizeof(record.measurements), "{}");
    }
}

bool factory_aggregate_pass(FactoryRunResult *result)
{
    if (result == nullptr) {
        return false;
    }
    result->overall_pass = std::all_of(
        result->tests.begin(), result->tests.end(),
        [](const FactoryTestRecord &record) {
            return record.status == FactoryTestStatus::kPass;
        });
    return result->overall_pass;
}

std::string factory_json_escape(const char *text)
{
    std::string escaped;
    if (text == nullptr) {
        return escaped;
    }
    for (const unsigned char value : std::string(text)) {
        switch (value) {
        case '"': escaped += "\\\""; break;
        case '\\': escaped += "\\\\"; break;
        case '\b': escaped += "\\b"; break;
        case '\f': escaped += "\\f"; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default:
            if (value < 0x20) {
                char unicode[8] = {};
                std::snprintf(unicode, sizeof(unicode), "\\u%04x", value);
                escaped += unicode;
            } else {
                escaped.push_back(static_cast<char>(value));
            }
        }
    }
    return escaped;
}

std::string factory_result_json(const FactoryRunResult &result)
{
    std::ostringstream json;
    json << "{\"schema\":" << result.schema
         << ",\"run_id\":\"" << factory_json_escape(result.run_id) << "\""
         << ",\"device\":\"" << factory_json_escape(result.device_name) << "\""
         << ",\"firmware\":\"" << factory_json_escape(result.firmware_version) << "\""
         << ",\"idf\":\"" << factory_json_escape(result.idf_version) << "\""
         << ",\"sta_mac\":\"" << factory_json_escape(result.sta_mac) << "\""
         << ",\"camera\":{\"configured\":\""
         << factory_camera_profile_name(result.camera_config)
         << "\",\"detected\":\"" << factory_json_escape(result.camera_detected)
         << "\",\"address\":" << static_cast<unsigned>(result.camera_address)
         << ",\"pid\":" << result.camera_pid << "}"
         << ",\"overall\":\"" << (result.overall_pass ? "PASS" : "FAIL") << "\""
         << ",\"duration_ms\":" << result.duration_ms
         << ",\"tests\":[";
    for (size_t i = 0; i < result.tests.size(); ++i) {
        const FactoryTestRecord &record = result.tests[i];
        if (i != 0) {
            json << ',';
        }
        json << "{\"id\":\"" << factory_test_id_name(record.id)
             << "\",\"status\":\"" << factory_test_status_name(record.status)
             << "\",\"attempts\":" << record.attempts
             << ",\"duration_ms\":" << record.duration_ms
             << ",\"error\":\"" << factory_json_escape(record.error_code)
             << "\",\"detail\":\"" << factory_json_escape(record.detail)
             << "\",\"measurements\":"
             << (record.measurements[0] == '{' ? record.measurements : "{}")
             << '}';
    }
    json << "]}";
    return json.str();
}
