#include "motion_behavior.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "app_face_follow.h"
#include "actuator_control.h"
#include "esp_log.h"
#include "motion_manager.h"

#define MOTION_BEHAVIOR_MANUAL_TTL_MS 120U
#define MOTION_BEHAVIOR_WEB_TTL_MS 300U
#define MOTION_BEHAVIOR_WEB_RAMP_MS 80U
#define MOTION_BEHAVIOR_VOICE_TTL_MS 900U
#define MOTION_BEHAVIOR_MCP_RAMP_MS 250U
#define MOTION_BEHAVIOR_DEFAULT_RAMP_MS 400U
#define MOTION_SPIN_360_DURATION_MS 3250U
#define MOTION_SPIN_360_RAMP_MS 500U

static const char *TAG = "MOTION_BEHAVIOR";
static bool s_ready;

static const char *skip_ascii_space(const char *text)
{
    while (text != NULL && (*text == ' ' || *text == '\t' ||
                            *text == '\r' || *text == '\n')) {
        ++text;
    }
    return text;
}

static bool command_matches(const char *text, const char *phrase)
{
    static const char *const prefixes[] = {
        "", "请", "请你", "小车", "请小车", "让小车", "让它",
        "请让它", "机器人", "让机器人", "请让机器人",
    };
    if (text == NULL || phrase == NULL) {
        return false;
    }

    const size_t phrase_length = strlen(phrase);
    const char *cursor = skip_ascii_space(text);

    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); ++i) {
        const size_t prefix_length = strlen(prefixes[i]);
        const char *command = cursor;
        if (strncmp(command, prefixes[i], prefix_length) != 0) {
            continue;
        }
        command = skip_ascii_space(command + prefix_length);
        if (strncmp(command, phrase, phrase_length) != 0) {
            continue;
        }
        command = skip_ascii_space(command + phrase_length);
        if (*command == '\0') {
            return true;
        }
    }
    return false;
}

static bool command_matches_any(const char *text,
                                const char *first,
                                const char *second,
                                const char *third)
{
    return command_matches(text, first) ||
           (second != NULL && command_matches(text, second)) ||
           (third != NULL && command_matches(text, third));
}

static motion_request_t make_request(float linear, float angular,
                                     uint32_t ttl_ms)
{
    return (motion_request_t) {
        .linear = linear,
        .angular = angular,
        .ttl_ms = ttl_ms,
        .ramp_ms = MOTION_BEHAVIOR_DEFAULT_RAMP_MS,
        .servo_pulse_us = ACTUATOR_SERVO_PULSE_CENTER_US,
        .servo_enabled = false,
    };
}

static motion_request_t make_web_request(float linear, float angular,
                                         int servo_pulse_us,
                                         bool servo_enabled)
{
    motion_request_t request = make_request(
        linear, angular, MOTION_BEHAVIOR_WEB_TTL_MS);
    request.ramp_ms = MOTION_BEHAVIOR_WEB_RAMP_MS;
    request.servo_pulse_us = servo_pulse_us;
    request.servo_enabled = servo_enabled;
    return request;
}

static motion_segment_t make_segment(float linear, float angular,
                                     uint32_t duration_ms, uint32_t ramp_ms,
                                     int servo_pulse_us, bool servo_enabled)
{
    return (motion_segment_t) {
        .linear = linear,
        .angular = angular,
        .duration_ms = duration_ms,
        .ramp_ms = ramp_ms,
        .servo_pulse_us = servo_pulse_us,
        .servo_enabled = servo_enabled,
    };
}

esp_err_t motion_behavior_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    const esp_err_t ret = motion_manager_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "motion unavailable: %s", esp_err_to_name(ret));
        return ret;
    }
    motion_manager_reset_pose();
    s_ready = true;
    return ESP_OK;
}

void motion_behavior_submit_manual(float linear, float angular)
{
    motion_behavior_submit_manual_control(linear, angular,
                                          ACTUATOR_SERVO_PULSE_CENTER_US,
                                          false);
}

void motion_behavior_submit_manual_control(float linear, float angular,
                                           int servo_pulse_us,
                                           bool servo_enabled)
{
    if (!s_ready) {
        return;
    }
    motion_request_t request = make_request(linear, angular,
                                            MOTION_BEHAVIOR_MANUAL_TTL_MS);
    request.servo_pulse_us = servo_pulse_us;
    request.servo_enabled = servo_enabled;
    if (motion_manager_submit(MOTION_SOURCE_TOUCH, &request) != ESP_OK) {
        ESP_LOGW(TAG, "manual motion request rejected");
    }
}

void motion_behavior_submit_manual_servo(int servo_pulse_us)
{
    motion_behavior_submit_manual_control(0.0f, 0.0f, servo_pulse_us, true);
}

void motion_behavior_release_manual(void)
{
    if (s_ready) {
        motion_manager_clear_source(MOTION_SOURCE_TOUCH);
    }
}

void motion_behavior_submit_web(float linear, float angular)
{
    motion_behavior_submit_web_control(linear, angular,
                                       ACTUATOR_SERVO_PULSE_CENTER_US, false);
}

void motion_behavior_submit_web_control(float linear, float angular,
                                        int servo_pulse_us,
                                        bool servo_enabled)
{
    if (!s_ready) {
        return;
    }
    const motion_request_t request = make_web_request(
        linear, angular, servo_pulse_us, servo_enabled);
    if (motion_manager_submit(MOTION_SOURCE_WEB, &request) != ESP_OK) {
        ESP_LOGW(TAG, "web motion request rejected");
    }
}

void motion_behavior_release_web(void)
{
    if (s_ready) {
        motion_manager_clear_source(MOTION_SOURCE_WEB);
    }
}

esp_err_t motion_behavior_submit_mcp(float linear,
                                     float angular,
                                     uint32_t duration_ms)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    /* A new MCP request replaces an older MCP sequence, such as a nod or a
     * previous spin, so an expired command cannot resume unexpectedly. */
    motion_manager_clear_source(MOTION_SOURCE_MCP);
    motion_request_t request = make_request(linear, angular, duration_ms);
    request.ramp_ms = MOTION_BEHAVIOR_MCP_RAMP_MS;
    return motion_manager_submit(MOTION_SOURCE_MCP, &request);
}

esp_err_t motion_behavior_submit_mcp_servo(int servo_pulse_us,
                                            uint32_t hold_ms)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    motion_manager_clear_source(MOTION_SOURCE_MCP);
    motion_request_t request = make_request(0.0f, 0.0f, hold_ms);
    request.ramp_ms = MOTION_BEHAVIOR_MCP_RAMP_MS;
    request.servo_pulse_us = servo_pulse_us;
    request.servo_enabled = true;
    return motion_manager_submit(MOTION_SOURCE_MCP, &request);
}

static esp_err_t start_named_for_source(motion_source_t source,
                                        motion_behavior_action_t action)
{
    motion_segment_t segments[MOTION_MAX_SEGMENTS];
    size_t count = 0;

    switch (action) {
    case MOTION_BEHAVIOR_ACTION_SPIN_360:
        segments[count++] = make_segment(0.0f, 1.0f,
                                         MOTION_SPIN_360_DURATION_MS,
                                         MOTION_SPIN_360_RAMP_MS,
                                         ACTUATOR_SERVO_PULSE_CENTER_US,
                                         false);
        break;
    case MOTION_BEHAVIOR_ACTION_GREETING:
        segments[count++] = make_segment(0.25f, 0.0f, 220U, 400U, 1600,
                                         true);
        segments[count++] = make_segment(0.0f, 0.0f, 180U, 250U, 1500,
                                         true);
        segments[count++] = make_segment(-0.20f, 0.0f, 180U, 250U, 1400,
                                         true);
        segments[count++] = make_segment(0.0f, 0.0f, 260U, 300U, 1500,
                                         true);
        break;
    case MOTION_BEHAVIOR_ACTION_WIGGLE:
        segments[count++] = make_segment(0.0f, 0.65f, 220U, 300U, 1500,
                                         false);
        segments[count++] = make_segment(0.0f, -0.65f, 440U, 300U, 1500,
                                         false);
        segments[count++] = make_segment(0.0f, 0.65f, 220U, 300U, 1500,
                                         false);
        break;
    case MOTION_BEHAVIOR_ACTION_NOD:
        segments[count++] = make_segment(0.0f, 0.0f, 300U, 300U, 1400,
                                         true);
        segments[count++] = make_segment(0.0f, 0.0f, 360U, 300U, 1600,
                                         true);
        segments[count++] = make_segment(0.0f, 0.0f, 300U, 300U, 1400,
                                         true);
        segments[count++] = make_segment(0.0f, 0.0f, 420U, 350U, 1500,
                                         true);
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }

    return motion_manager_start_sequence(source, segments, count);
}

esp_err_t motion_behavior_start_spin_360(bool clockwise)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    motion_manager_clear_source(MOTION_SOURCE_MCP);
    const motion_segment_t segment = make_segment(
        0.0f, clockwise ? -1.0f : 1.0f, MOTION_SPIN_360_DURATION_MS,
        MOTION_SPIN_360_RAMP_MS, ACTUATOR_SERVO_PULSE_CENTER_US, false);
    return motion_manager_start_sequence(MOTION_SOURCE_MCP, &segment, 1);
}

esp_err_t motion_behavior_start_named(motion_behavior_action_t action)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    return start_named_for_source(MOTION_SOURCE_MCP, action);
}

void motion_behavior_on_user_text(const char *text)
{
    if (!s_ready || text == NULL || text[0] == '\0') {
        return;
    }

    if (command_matches_any(text, "停止", "停下", "急停")) {
        (void)app_face_follow_stop();
        motion_manager_stop_all();
        return;
    }
    if (command_matches_any(text, "转一圈", "旋转一圈", "转圈圈") ||
        command_matches_any(text, "转个圈", "绕一圈", "转一圈儿") ||
        command_matches(text, "转圈")) {
        (void)start_named_for_source(MOTION_SOURCE_VOICE,
                                     MOTION_BEHAVIOR_ACTION_SPIN_360);
        return;
    }
    if (command_matches_any(text, "打招呼", "打个招呼", "问候")) {
        (void)start_named_for_source(MOTION_SOURCE_VOICE,
                                     MOTION_BEHAVIOR_ACTION_GREETING);
        return;
    }

    motion_request_t request;
    if (command_matches_any(text, "前进", "向前", NULL)) {
        request = make_request(0.70f, 0.0f, MOTION_BEHAVIOR_VOICE_TTL_MS);
    } else if (command_matches_any(text, "后退", "向后", NULL)) {
        request = make_request(-0.60f, 0.0f, MOTION_BEHAVIOR_VOICE_TTL_MS);
    } else if (command_matches_any(text, "左转", "向左", NULL)) {
        request = make_request(0.0f, 0.70f, MOTION_BEHAVIOR_VOICE_TTL_MS);
    } else if (command_matches_any(text, "右转", "向右", NULL)) {
        request = make_request(0.0f, -0.70f, MOTION_BEHAVIOR_VOICE_TTL_MS);
    } else {
        return;
    }

    if (motion_manager_submit(MOTION_SOURCE_VOICE, &request) != ESP_OK) {
        ESP_LOGW(TAG, "voice motion request rejected");
    }
}

static void start_expression(const motion_segment_t *segments, size_t count)
{
    if (!s_ready || segments == NULL || count == 0) {
        return;
    }
    if (motion_manager_start_sequence(MOTION_SOURCE_EXPRESSION,
                                      segments, count) != ESP_OK) {
        ESP_LOGW(TAG, "expression motion request rejected");
    }
}

void motion_behavior_on_emotion(const char *emotion)
{
    if (!s_ready || emotion == NULL) {
        return;
    }

    /* Neutral is the normal idle/wake state. It must not move the head back
     * to center when XiaoZhi opens or closes an audio conversation. */
    if (strcmp(emotion, "neutral") == 0) {
        return;
    }

    motion_segment_t segments[3];
    size_t count = 0;
    if (strcmp(emotion, "happy") == 0) {
        /* XiaoZhi ready pose: move directly to the requested raised-head
         * position so startup cannot finish at the 1500us default. */
        segments[count++] = make_segment(0.0f, 0.0f, 700U, 400U, 1800,
                                         true);
    } else if (strcmp(emotion, "sad") == 0 ||
               strcmp(emotion, "crying") == 0) {
        segments[count++] = make_segment(0.0f, 0.0f, 260U, 350U, 1400,
                                         true);
        segments[count++] = make_segment(0.0f, 0.0f, 420U, 350U, 1500,
                                         true);
    } else if (strcmp(emotion, "shocked") == 0) {
        segments[count++] = make_segment(0.0f, 0.0f, 140U, 250U, 1750,
                                         true);
        segments[count++] = make_segment(0.0f, 0.0f, 320U, 300U, 1500,
                                         true);
    } else if (strcmp(emotion, "confused") == 0) {
        /* Answer motion uses the full calibrated servo range while retaining
         * the manager's gentle slew rate. Always settle at the ready pose so
         * the next conversational turn does not return to center. */
        segments[count++] = make_segment(0.0f, 0.0f, 900U, 300U, 1100,
                                         true);
        segments[count++] = make_segment(0.0f, 0.0f, 1200U, 300U, 1900,
                                         true);
        segments[count++] = make_segment(0.0f, 0.0f, 500U, 300U, 1800,
                                         true);
    } else if (strcmp(emotion, "sleepy") == 0) {
        segments[count++] = make_segment(0.0f, 0.0f, 420U, 450U, 1400,
                                         true);
        segments[count++] = make_segment(0.0f, 0.0f, 520U, 450U, 1500,
                                         true);
    } else if (strcmp(emotion, "angry") == 0 ||
               strcmp(emotion, "winking") == 0) {
        segments[count++] = make_segment(0.0f, 0.0f, 180U, 250U, 1600,
                                         true);
        segments[count++] = make_segment(0.0f, 0.0f, 220U, 300U, 1500,
                                         true);
    } else {
        return;
    }

    start_expression(segments, count);
}
