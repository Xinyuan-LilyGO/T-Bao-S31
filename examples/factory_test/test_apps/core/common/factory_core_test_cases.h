#pragma once

#include <cstdio>
#include <cstring>
#include <string>

#include "factory_core.h"

static int s_cleanup_calls = 0;

static void cleanup_counter()
{
    ++s_cleanup_calls;
}

static void factory_core_case_parser()
{
    FactoryAction action = {};
    char error[48] = {};

    FT_ASSERT_TRUE(factory_parse_command("RUN\r\n", &action, error, sizeof(error)));
    FT_ASSERT_EQ_INT(
        static_cast<int>(FactoryActionType::kRun), static_cast<int>(action.type));

    FT_ASSERT_TRUE(factory_parse_command(
        "camera ov3660", &action, error, sizeof(error)));
    FT_ASSERT_EQ_INT(
        static_cast<int>(FactoryActionType::kCameraOv3660),
        static_cast<int>(action.type));

    FT_ASSERT_TRUE(factory_parse_command(
        "PASS speaker", &action, error, sizeof(error)));
    FT_ASSERT_EQ_INT(
        static_cast<int>(FactoryActionType::kManualPass),
        static_cast<int>(action.type));
    FT_ASSERT_EQ_INT(
        static_cast<int>(FactoryTestId::kSpeaker),
        static_cast<int>(action.test_id));

    FT_ASSERT_TRUE(factory_parse_command(
        "RETRY camera", &action, error, sizeof(error)));
    FT_ASSERT_EQ_INT(
        static_cast<int>(FactoryActionType::kRetry),
        static_cast<int>(action.type));

    FT_ASSERT_TRUE(factory_parse_command(
        "RETEST ALL", &action, error, sizeof(error)));
    FT_ASSERT_EQ_INT(
        static_cast<int>(FactoryActionType::kRetestAll),
        static_cast<int>(action.type));

    FT_ASSERT_FALSE(factory_parse_command(
        "CAMERA OV5640", &action, error, sizeof(error)));
    FT_ASSERT_STREQ("BAD_CAMERA_PROFILE", error);
    FT_ASSERT_FALSE(factory_parse_command(
        "PASS wifi extra", &action, error, sizeof(error)));
    FT_ASSERT_STREQ("BAD_COMMAND", error);
}

static void factory_core_case_state_transitions()
{
    FT_ASSERT_TRUE(factory_status_transition_valid(
        FactoryTestStatus::kPending, FactoryTestStatus::kRunning));
    FT_ASSERT_TRUE(factory_status_transition_valid(
        FactoryTestStatus::kRunning, FactoryTestStatus::kWaitingManual));
    FT_ASSERT_TRUE(factory_status_transition_valid(
        FactoryTestStatus::kWaitingManual, FactoryTestStatus::kRunning));
    FT_ASSERT_TRUE(factory_status_transition_valid(
        FactoryTestStatus::kWaitingManual, FactoryTestStatus::kPass));
    FT_ASSERT_TRUE(factory_status_transition_valid(
        FactoryTestStatus::kFail, FactoryTestStatus::kRunning));
    FT_ASSERT_FALSE(factory_status_transition_valid(
        FactoryTestStatus::kPending, FactoryTestStatus::kPass));
    FT_ASSERT_FALSE(factory_status_transition_valid(
        FactoryTestStatus::kPass, FactoryTestStatus::kFail));
}

static void factory_core_case_manual_decisions()
{
    FactoryAction action = {};
    action.type = FactoryActionType::kRetry;
    action.test_id = FactoryTestId::kCamera;
    FT_ASSERT_EQ_INT(
        static_cast<int>(FactoryManualDecision::kRetry),
        static_cast<int>(factory_manual_decision(
            FactoryTestId::kCamera, action, false)));

    action.type = FactoryActionType::kManualPass;
    FT_ASSERT_EQ_INT(
        static_cast<int>(FactoryManualDecision::kPass),
        static_cast<int>(factory_manual_decision(
            FactoryTestId::kCamera, action, false)));

    action.type = FactoryActionType::kManualFail;
    FT_ASSERT_EQ_INT(
        static_cast<int>(FactoryManualDecision::kFail),
        static_cast<int>(factory_manual_decision(
            FactoryTestId::kCamera, action, false)));

    action.test_id = FactoryTestId::kSpeaker;
    FT_ASSERT_EQ_INT(
        static_cast<int>(FactoryManualDecision::kIgnore),
        static_cast<int>(factory_manual_decision(
            FactoryTestId::kCamera, action, false)));
    FT_ASSERT_EQ_INT(
        static_cast<int>(FactoryManualDecision::kTimeout),
        static_cast<int>(factory_manual_decision(
            FactoryTestId::kCamera, action, true)));
}

static void factory_core_case_cleanup_guard()
{
    s_cleanup_calls = 0;
    {
        FactoryCleanupGuard guard(cleanup_counter);
    }
    FT_ASSERT_EQ_INT(1, s_cleanup_calls);

    {
        FactoryCleanupGuard guard(cleanup_counter);
        guard.run_now();
        guard.run_now();
    }
    FT_ASSERT_EQ_INT(2, s_cleanup_calls);

    {
        FactoryCleanupGuard guard(cleanup_counter);
        guard.dismiss();
    }
    FT_ASSERT_EQ_INT(2, s_cleanup_calls);
}

static void factory_core_case_thresholds()
{
    FT_ASSERT_TRUE(factory_mic_threshold_pass(
        -40.0f, -42.0f, -45.0f, true, true, false));
    FT_ASSERT_FALSE(factory_mic_threshold_pass(
        -46.0f, -42.0f, -45.0f, true, true, false));
    FT_ASSERT_FALSE(factory_mic_threshold_pass(
        -40.0f, -42.0f, -45.0f, false, true, false));
    FT_ASSERT_FALSE(factory_mic_threshold_pass(
        -40.0f, -42.0f, -45.0f, true, true, true));

    FT_ASSERT_TRUE(factory_charger_threshold_pass(
        5000, 7600, 120, false, true, true, 0));
    FT_ASSERT_TRUE(factory_charger_threshold_pass(
        5000, 8400, 0, true, false, true, 0));
    FT_ASSERT_FALSE(factory_charger_threshold_pass(
        5000, 4100, 120, false, true, true, 0));
    FT_ASSERT_FALSE(factory_charger_threshold_pass(
        4300, 7600, 120, false, true, true, 0));
    FT_ASSERT_FALSE(factory_charger_threshold_pass(
        5000, 7600, 0, false, true, true, 0));
    FT_ASSERT_FALSE(factory_charger_threshold_pass(
        5000, 7600, 120, false, true, true, 0x10));
}

static void factory_core_case_aggregation_and_reset()
{
    FactoryRunResult result = {};
    std::snprintf(result.device_name, sizeof(result.device_name), "T-Bao-S31");
    factory_reset_run(&result);
    for (size_t index = 0; index < result.tests.size(); ++index) {
        FT_ASSERT_EQ_INT(static_cast<int>(index), static_cast<int>(result.tests[index].id));
        FT_ASSERT_EQ_INT(
            static_cast<int>(FactoryTestStatus::kPending),
            static_cast<int>(result.tests[index].status));
        result.tests[index].status = FactoryTestStatus::kPass;
    }
    FT_ASSERT_TRUE(factory_aggregate_pass(&result));
    FT_ASSERT_TRUE(result.overall_pass);

    result.tests[3].status = FactoryTestStatus::kFail;
    FT_ASSERT_FALSE(factory_aggregate_pass(&result));
    FT_ASSERT_FALSE(result.overall_pass);

    result.camera_address = 0x30;
    result.camera_pid = 0x2642;
    result.duration_ms = 1234;
    factory_reset_run(&result);
    FT_ASSERT_EQ_INT(0, result.camera_address);
    FT_ASSERT_EQ_INT(0, result.camera_pid);
    FT_ASSERT_EQ_INT(0, result.duration_ms);
    FT_ASSERT_STREQ("UNKNOWN", result.camera_detected);
    FT_ASSERT_STREQ("T-Bao-S31", result.device_name);
}

static void factory_core_case_json()
{
    FT_ASSERT_STREQ(
        "quote\\\" slash\\\\ line\\n tab\\t",
        factory_json_escape("quote\" slash\\ line\n tab\t").c_str());

    FactoryRunResult result = {};
    result.schema = 1;
    std::snprintf(result.run_id, sizeof(result.run_id), "run-1");
    std::snprintf(result.device_name, sizeof(result.device_name), "T-Bao-\"S31");
    std::snprintf(result.firmware_version, sizeof(result.firmware_version), "1.0.0");
    std::snprintf(result.idf_version, sizeof(result.idf_version), "6.1");
    std::snprintf(result.sta_mac, sizeof(result.sta_mac), "00:11:22:33:44:55");
    result.camera_config = FactoryCameraProfile::kOv2640;
    std::snprintf(result.camera_detected, sizeof(result.camera_detected), "OV2640");
    result.camera_address = 0x30;
    result.camera_pid = 0x2642;
    factory_reset_run(&result);
    result.camera_config = FactoryCameraProfile::kOv2640;
    std::snprintf(result.camera_detected, sizeof(result.camera_detected), "OV2640");
    result.camera_address = 0x30;
    result.camera_pid = 0x2642;
    for (FactoryTestRecord &record : result.tests) {
        record.status = FactoryTestStatus::kPass;
    }
    std::snprintf(
        result.tests[0].measurements,
        sizeof(result.tests[0].measurements), "{\"value\":1}");
    std::snprintf(
        result.tests[1].measurements,
        sizeof(result.tests[1].measurements), "not-json");
    factory_aggregate_pass(&result);

    const std::string json = factory_result_json(result);
    FT_ASSERT_CONTAINS("\"device\":\"T-Bao-\\\"S31\"", json.c_str());
    FT_ASSERT_CONTAINS("\"overall\":\"PASS\"", json.c_str());
    FT_ASSERT_CONTAINS("\"measurements\":{\"value\":1}", json.c_str());
    FT_ASSERT_CONTAINS("\"id\":\"microphone\"", json.c_str());
    FT_ASSERT_CONTAINS("\"measurements\":{}", json.c_str());
}
