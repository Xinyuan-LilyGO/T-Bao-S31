#include "motion_manager.h"

#include <math.h>
#include <string.h>

#include "actuator_control.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define MOTION_MANAGER_PERIOD_MS 20
#define MOTION_MAX_SEGMENT_MS 10000U
#define MOTION_LINEAR_SPEED_CM_S 4.0f
#define MOTION_ANGULAR_SPEED_DEG_S 120.0f
/* The diagonal wheel wiring is calibrated from the physical joystick test:
 * equal signed outputs drive forward, while opposite outputs turn. */
#define MOTION_TURN_GAIN (-1.0f)
#define MOTION_MOTOR_A_SIGN 1.0f
#define MOTION_MOTOR_B_SIGN 1.0f
#define MOTION_MOTOR_MAX_PERCENT 100.0f
#define MOTION_MOTOR_MIN_PERCENT 18.0f
/* Face-follow commands deliberately use a small angular range (max 0.10).
 * Increase breakaway torque continuously with the requested turn instead of
 * switching abruptly at one angular threshold. */
#define MOTION_FOLLOW_TURN_KICK_ANGULAR 0.08f
#define MOTION_FOLLOW_TURN_MIN_PERCENT 28.0f
#define MOTION_ACCEL_MS 400U
#define MOTION_BRAKE_MS 250U
#define MOTION_REVERSE_DEADTIME_MS 80U
#define MOTION_SERVO_SLEW_US_PER_SEC 480.0f
#define MOTION_DEFAULT_SERVO_PULSE_US 1800
#define MOTION_SERVO_DIAG_INTERVAL_MS 250U
#define MOTION_SERVO_DIAG_DELTA_US 8
#define MOTION_MOTOR_DIAG_INTERVAL_MS 250U
#define MOTION_MOTOR_DIAG_DELTA_PERCENT 2

static const char *TAG = "MOTION_MANAGER";

typedef struct {
    bool valid;
    motion_request_t request;
    uint32_t expires_at_ms;
} request_slot_t;

typedef struct {
    bool valid;
    motion_source_t source;
    motion_segment_t segments[MOTION_MAX_SEGMENTS];
    size_t count;
    size_t index;
    uint32_t elapsed_ms;
} sequence_state_t;

typedef struct {
    bool valid;
    motion_source_t source;
    motion_twist_t twist;
    uint32_t ramp_ms;
    int servo_pulse_us;
    bool servo_enabled;
} selected_command_t;

static SemaphoreHandle_t s_mutex;
static TaskHandle_t s_task;
static bool s_running;
static bool s_available;
static bool s_faulted;
static float s_current_motor_a;
static float s_current_motor_b;
static float s_current_servo_pulse_us;
static uint32_t s_reverse_wait_a;
static uint32_t s_reverse_wait_b;
static request_slot_t s_requests[MOTION_SOURCE_COUNT];
static sequence_state_t s_sequence;
static motion_state_t s_state;
static bool s_servo_diag_initialized;
static motion_source_t s_servo_diag_source;
static int s_servo_diag_target_us;
static int s_servo_diag_output_us;
static bool s_servo_diag_enabled;
static uint32_t s_servo_diag_last_ms;
static bool s_motor_diag_initialized;
static motion_source_t s_motor_diag_source;
static bool s_motor_diag_has_command;
static int s_motor_diag_target_a;
static int s_motor_diag_target_b;
static int s_motor_diag_output_a;
static int s_motor_diag_output_b;
static uint32_t s_motor_diag_reverse_wait_a;
static uint32_t s_motor_diag_reverse_wait_b;
static uint32_t s_motor_diag_last_ms;

static const motion_source_t s_source_priority[] = {
    MOTION_SOURCE_SAFETY,
    MOTION_SOURCE_WEB,
    MOTION_SOURCE_TOUCH,
    MOTION_SOURCE_MCP,
    MOTION_SOURCE_FOLLOW,
    MOTION_SOURCE_VOICE,
    MOTION_SOURCE_EXPRESSION,
};

static float clampf(float value, float minimum, float maximum)
{
    if (value < minimum) {
        return minimum;
    }
    if (value > maximum) {
        return maximum;
    }
    return value;
}

static bool source_valid(motion_source_t source)
{
    return source >= MOTION_SOURCE_EXPRESSION &&
           source < MOTION_SOURCE_COUNT;
}

static const char *motion_source_name(motion_source_t source)
{
    switch (source) {
    case MOTION_SOURCE_EXPRESSION:
        return "expression";
    case MOTION_SOURCE_VOICE:
        return "voice";
    case MOTION_SOURCE_MCP:
        return "mcp";
    case MOTION_SOURCE_TOUCH:
        return "touch";
    case MOTION_SOURCE_WEB:
        return "web";
    case MOTION_SOURCE_SAFETY:
        return "safety";
    case MOTION_SOURCE_FOLLOW:
        return "follow";
    case MOTION_SOURCE_NONE:
    default:
        return "none";
    }
}

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static bool request_active(const request_slot_t *slot, uint32_t current_ms)
{
    return slot->valid && (int32_t)(current_ms - slot->expires_at_ms) < 0;
}

static void clear_expired_requests_locked(uint32_t current_ms)
{
    for (int i = 0; i < MOTION_SOURCE_COUNT; ++i) {
        if (s_requests[i].valid && !request_active(&s_requests[i], current_ms)) {
            s_requests[i].valid = false;
        }
    }
}

static void clear_all_requests_locked(void)
{
    memset(s_requests, 0, sizeof(s_requests));
    memset(&s_sequence, 0, sizeof(s_sequence));
}

static bool select_command_locked(uint32_t current_ms,
                                  selected_command_t *selected)
{
    memset(selected, 0, sizeof(*selected));
    selected->source = MOTION_SOURCE_NONE;
    selected->servo_pulse_us = MOTION_DEFAULT_SERVO_PULSE_US;
    clear_expired_requests_locked(current_ms);

    for (size_t i = 0; i < sizeof(s_source_priority) /
                            sizeof(s_source_priority[0]); ++i) {
        const motion_source_t source = s_source_priority[i];

        /* Sequences are normally considered after request slots so a live
         * manual request remains authoritative. An MCP sequence is still an
         * explicit user command, so let it outrank the autonomous follow
         * heartbeat at the point where follow would otherwise win. */
        if (source == MOTION_SOURCE_FOLLOW && s_sequence.valid &&
            s_sequence.source == MOTION_SOURCE_MCP &&
            s_sequence.index < s_sequence.count) {
            const motion_segment_t *segment =
                &s_sequence.segments[s_sequence.index];
            selected->valid = true;
            selected->source = s_sequence.source;
            selected->twist = (motion_twist_t){segment->linear,
                                               segment->angular};
            selected->ramp_ms = segment->ramp_ms;
            selected->servo_pulse_us = segment->servo_pulse_us;
            selected->servo_enabled = segment->servo_enabled;
            return true;
        }
        if (!request_active(&s_requests[source], current_ms)) {
            continue;
        }
        const motion_request_t *request = &s_requests[source].request;
        selected->valid = true;
        selected->source = (motion_source_t)source;
        selected->twist = (motion_twist_t){request->linear, request->angular};
        selected->ramp_ms = request->ramp_ms;
        selected->servo_pulse_us = request->servo_pulse_us;
        selected->servo_enabled = request->servo_enabled;
        return true;
    }

    if (s_sequence.valid && s_sequence.index < s_sequence.count) {
        const motion_segment_t *segment = &s_sequence.segments[s_sequence.index];
        selected->valid = true;
        selected->source = s_sequence.source;
        selected->twist = (motion_twist_t){segment->linear, segment->angular};
        selected->ramp_ms = segment->ramp_ms;
        selected->servo_pulse_us = segment->servo_pulse_us;
        selected->servo_enabled = segment->servo_enabled;
        return true;
    }

    s_sequence.valid = false;
    return false;
}

static bool select_servo_command_locked(uint32_t current_ms,
                                        selected_command_t *selected)
{
    memset(selected, 0, sizeof(*selected));
    selected->source = MOTION_SOURCE_NONE;
    selected->servo_pulse_us = MOTION_DEFAULT_SERVO_PULSE_US;

    for (size_t i = 0; i < sizeof(s_source_priority) /
                            sizeof(s_source_priority[0]); ++i) {
        const motion_source_t source = s_source_priority[i];

        /* An MCP sequence is explicitly allowed to outrank follow at the
         * same point where the wheel selector gives it priority. A segment
         * without servo control is only a wheel command, so keep searching
         * for a lower-priority source that still owns the camera. */
        if (source == MOTION_SOURCE_FOLLOW && s_sequence.valid &&
            s_sequence.source == MOTION_SOURCE_MCP &&
            s_sequence.index < s_sequence.count) {
            const motion_segment_t *segment =
                &s_sequence.segments[s_sequence.index];
            if (segment->servo_enabled) {
                selected->valid = true;
                selected->source = s_sequence.source;
                selected->servo_pulse_us = segment->servo_pulse_us;
                selected->servo_enabled = true;
                return true;
            }
        }

        if (!request_active(&s_requests[source], current_ms) ||
            !s_requests[source].request.servo_enabled) {
            continue;
        }
        const motion_request_t *request = &s_requests[source].request;
        selected->valid = true;
        selected->source = source;
        selected->servo_pulse_us = request->servo_pulse_us;
        selected->servo_enabled = true;
        return true;
    }

    if (s_sequence.valid && s_sequence.index < s_sequence.count) {
        const motion_segment_t *segment = &s_sequence.segments[s_sequence.index];
        if (segment->servo_enabled) {
            selected->valid = true;
            selected->source = s_sequence.source;
            selected->servo_pulse_us = segment->servo_pulse_us;
            selected->servo_enabled = true;
            return true;
        }
    }

    return false;
}

static float motor_min_percent_for_command(
    const selected_command_t *selected, motion_twist_t requested)
{
    if (selected != NULL && selected->valid &&
        selected->source == MOTION_SOURCE_FOLLOW) {
        const float turn_ratio = clampf(
            fabsf(requested.angular) / MOTION_FOLLOW_TURN_KICK_ANGULAR,
            0.0f, 1.0f);
        return MOTION_MOTOR_MIN_PERCENT +
               (MOTION_FOLLOW_TURN_MIN_PERCENT - MOTION_MOTOR_MIN_PERCENT) *
                   turn_ratio;
    }
    return MOTION_MOTOR_MIN_PERCENT;
}

static void maybe_log_motor_diagnostics(
    uint32_t current_ms,
    bool has_command,
    motion_source_t selected_source,
    motion_twist_t requested,
    const motion_wheel_mix_t *mix,
    float minimum_percent,
    int target_a,
    int target_b,
    int output_a,
    int output_b)
{
    if (mix == NULL) {
        return;
    }

    const bool source_changed =
        !s_motor_diag_initialized || selected_source != s_motor_diag_source;
    const bool command_changed =
        !s_motor_diag_initialized || has_command != s_motor_diag_has_command;
    const bool target_changed =
        !s_motor_diag_initialized ||
        fabsf((float)(target_a - s_motor_diag_target_a)) >=
            (float)MOTION_MOTOR_DIAG_DELTA_PERCENT ||
        fabsf((float)(target_b - s_motor_diag_target_b)) >=
            (float)MOTION_MOTOR_DIAG_DELTA_PERCENT;
    const bool output_changed =
        !s_motor_diag_initialized ||
        fabsf((float)(output_a - s_motor_diag_output_a)) >=
            (float)MOTION_MOTOR_DIAG_DELTA_PERCENT ||
        fabsf((float)(output_b - s_motor_diag_output_b)) >=
            (float)MOTION_MOTOR_DIAG_DELTA_PERCENT;
    const bool reverse_wait_changed =
        !s_motor_diag_initialized ||
        s_reverse_wait_a != s_motor_diag_reverse_wait_a ||
        s_reverse_wait_b != s_motor_diag_reverse_wait_b;
    const bool important_change = source_changed || command_changed ||
                                  target_changed || output_changed ||
                                  reverse_wait_changed;
    if (!important_change) {
        return;
    }
    if (s_motor_diag_initialized && !source_changed && !command_changed &&
        (uint32_t)(current_ms - s_motor_diag_last_ms) <
            MOTION_MOTOR_DIAG_INTERVAL_MS) {
        return;
    }

    s_motor_diag_initialized = true;
    s_motor_diag_source = selected_source;
    s_motor_diag_has_command = has_command;
    s_motor_diag_target_a = target_a;
    s_motor_diag_target_b = target_b;
    s_motor_diag_output_a = output_a;
    s_motor_diag_output_b = output_b;
    s_motor_diag_reverse_wait_a = s_reverse_wait_a;
    s_motor_diag_reverse_wait_b = s_reverse_wait_b;
    s_motor_diag_last_ms = current_ms;
    ESP_LOGI(TAG,
             "motor_diag selected_source=%s has_command=%d "
             "requested=(linear=%.3f angular=%.3f) "
             "mix=(a=%.3f b=%.3f) min=%.0f "
             "target=(a=%d b=%d) output=(a=%d b=%d) "
             "reverse_wait=(a=%ums b=%ums)",
             motion_source_name(selected_source), has_command ? 1 : 0,
             requested.linear, requested.angular, mix->motor_a, mix->motor_b,
             minimum_percent, target_a, target_b, output_a, output_b,
             (unsigned)s_reverse_wait_a, (unsigned)s_reverse_wait_b);
}

static float ramp_delta(float target, uint32_t ramp_ms)
{
    const uint32_t effective_ms = ramp_ms == 0 ? MOTION_ACCEL_MS : ramp_ms;
    if (target == 0.0f) {
        return 100.0f * MOTION_MANAGER_PERIOD_MS / MOTION_BRAKE_MS;
    }
    return clampf(100.0f * MOTION_MANAGER_PERIOD_MS / effective_ms,
                  0.1f, 100.0f);
}

static float ramp_motor(float current,
                        float target,
                        uint32_t ramp_ms,
                        uint32_t *reverse_wait_ms)
{
    const bool opposite = (current > 0.5f && target < -0.5f) ||
                          (current < -0.5f && target > 0.5f);
    if (opposite) {
        if (fabsf(current) > 0.5f) {
            if (*reverse_wait_ms == 0) {
                *reverse_wait_ms = MOTION_REVERSE_DEADTIME_MS;
            }
            return motion_math_ramp_toward(
                current, 0.0f, ramp_delta(0.0f, ramp_ms));
        }
        if (*reverse_wait_ms > 0) {
            *reverse_wait_ms = *reverse_wait_ms > MOTION_MANAGER_PERIOD_MS
                                   ? *reverse_wait_ms - MOTION_MANAGER_PERIOD_MS
                                   : 0;
            return 0.0f;
        }
    } else if (target == 0.0f ||
               (current == 0.0f && target == 0.0f) ||
               (current > 0.0f && target >= 0.0f) ||
               (current < 0.0f && target <= 0.0f)) {
        *reverse_wait_ms = 0;
    }

    return motion_math_ramp_toward(current, target,
                                   ramp_delta(target, ramp_ms));
}

static float ramp_servo_pulse(float current, int target)
{
    const float max_delta = MOTION_SERVO_SLEW_US_PER_SEC *
                            ((float)MOTION_MANAGER_PERIOD_MS / 1000.0f);
    return motion_math_ramp_toward(current, (float)target, max_delta);
}

static void maybe_log_servo_diagnostics(uint32_t current_ms,
                                        bool has_command,
                                        motion_source_t selected_source,
                                        int servo_target_us,
                                        int servo_output_us,
                                        bool servo_enabled)
{
    const bool source_changed =
        !s_servo_diag_initialized || selected_source != s_servo_diag_source;
    const bool enabled_changed =
        !s_servo_diag_initialized || servo_enabled != s_servo_diag_enabled;
    const bool target_changed =
        !s_servo_diag_initialized ||
        fabsf((float)(servo_target_us - s_servo_diag_target_us)) >=
            (float)MOTION_SERVO_DIAG_DELTA_US;
    const bool output_changed =
        !s_servo_diag_initialized ||
        fabsf((float)(servo_output_us - s_servo_diag_output_us)) >=
            (float)MOTION_SERVO_DIAG_DELTA_US;
    const bool important_change = source_changed || enabled_changed ||
                                  target_changed || output_changed;
    if (!important_change) {
        return;
    }
    if (s_servo_diag_initialized && !source_changed && !enabled_changed &&
        (uint32_t)(current_ms - s_servo_diag_last_ms) <
            MOTION_SERVO_DIAG_INTERVAL_MS) {
        return;
    }

    s_servo_diag_initialized = true;
    s_servo_diag_source = selected_source;
    s_servo_diag_target_us = servo_target_us;
    s_servo_diag_output_us = servo_output_us;
    s_servo_diag_enabled = servo_enabled;
    s_servo_diag_last_ms = current_ms;
    ESP_LOGI(TAG,
             "servo_diag selected_source=%s has_command=%d target_us=%d "
             "output_us=%d slew_us=%.1f servo_enabled=%d",
             motion_source_name(selected_source), has_command ? 1 : 0,
             servo_target_us, servo_output_us, s_current_servo_pulse_us,
             servo_enabled ? 1 : 0);
}

static void mark_fault_locked(void)
{
    const int held_servo_pulse_us = (int)lroundf(s_current_servo_pulse_us);
    clear_all_requests_locked();
    s_current_motor_a = 0.0f;
    s_current_motor_b = 0.0f;
    s_state.current_motor_a_percent = 0;
    s_state.current_motor_b_percent = 0;
    s_state.current_servo_pulse_us = held_servo_pulse_us;
    s_state.servo_enabled = false;
    s_state.target_motor_a_percent = 0;
    s_state.target_motor_b_percent = 0;
    s_state.active_source = MOTION_SOURCE_NONE;
    s_state.sequence_active = false;
    s_state.boundary_limited = false;
    s_state.available = false;
    s_state.faulted = true;
    s_state.failsafe_active = true;
    s_faulted = true;
}

static void manager_step(void)
{
    if (s_mutex == NULL || xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    if (!s_running || !s_available || s_faulted) {
        xSemaphoreGive(s_mutex);
        return;
    }

    const uint32_t current_ms = now_ms();
    selected_command_t selected;
    const bool has_command = select_command_locked(current_ms, &selected);
    selected_command_t selected_servo;
    const bool has_servo_command =
        select_servo_command_locked(current_ms, &selected_servo);
    const motion_twist_t requested = has_command
                                        ? selected.twist
                                        : (motion_twist_t){0.0f, 0.0f};
    /* The pose is telemetry only. There is deliberately no software boundary
     * clamp here: without encoders or edge sensors it cannot be a reliable
     * fall-prevention mechanism. */
    const motion_twist_t limited = requested;
    const bool boundary_limited = false;

    motion_wheel_mix_t mix;
    motion_math_mix_wheels(limited.linear, limited.angular,
                           MOTION_TURN_GAIN,
                           MOTION_MOTOR_A_SIGN, MOTION_MOTOR_B_SIGN, &mix);
    const float motor_min_percent =
        motor_min_percent_for_command(&selected, limited);
    const int target_a = motion_math_normalized_to_percent(
        mix.motor_a, motor_min_percent, MOTION_MOTOR_MAX_PERCENT);
    const int target_b = motion_math_normalized_to_percent(
        mix.motor_b, motor_min_percent, MOTION_MOTOR_MAX_PERCENT);
    const uint32_t ramp_ms = has_command ? selected.ramp_ms : MOTION_BRAKE_MS;
    s_current_motor_a = ramp_motor(s_current_motor_a, (float)target_a,
                                   ramp_ms, &s_reverse_wait_a);
    s_current_motor_b = ramp_motor(s_current_motor_b, (float)target_b,
                                   ramp_ms, &s_reverse_wait_b);

    const int output_a = (int)lroundf(s_current_motor_a);
    const int output_b = (int)lroundf(s_current_motor_b);
    /* Idle and motor-only commands must not synthesize a center command. Keep
     * the last servo target and PWM hold once a source has claimed the servo,
     * so a page transition or a no-face interval cannot make the head drift
     * before the next real tracking sample arrives. */
    /* Wheel arbitration and camera arbitration are independent. A manual
     * wheel request with servo_enabled=false means "I do not control the
     * camera", not "turn off the active face-follow servo". */
    const bool servo_command = has_servo_command;
    const int servo_target = servo_command
                                 ? selected_servo.servo_pulse_us
                                 : (int)lroundf(s_current_servo_pulse_us);
    if (servo_command) {
        s_current_servo_pulse_us = ramp_servo_pulse(
            s_current_servo_pulse_us, servo_target);
    }
    const int servo_output = (int)lroundf(clampf(
        s_current_servo_pulse_us,
        (float)ACTUATOR_SERVO_PULSE_MIN_US,
        (float)ACTUATOR_SERVO_PULSE_MAX_US));
    const bool servo_enabled = servo_command || s_state.servo_enabled;

    if (has_command) {
        motion_math_integrate_pose(&s_state.pose, limited,
                                   MOTION_LINEAR_SPEED_CM_S,
                                   MOTION_ANGULAR_SPEED_DEG_S,
                                   (float)MOTION_MANAGER_PERIOD_MS / 1000.0f);
    }

    s_state.requested_twist = requested;
    s_state.limited_twist = limited;
    s_state.current_motor_a_percent = output_a;
    s_state.current_motor_b_percent = output_b;
    /* Keep telemetry and the next controller handoff aligned with the pulse
     * that was actually applied after slew limiting. */
    s_state.current_servo_pulse_us = servo_output;
    s_state.servo_enabled = servo_enabled;
    s_state.target_motor_a_percent = target_a;
    s_state.target_motor_b_percent = target_b;
    s_state.boundary_limited = boundary_limited;
    s_state.active_source = has_command ? selected.source : MOTION_SOURCE_NONE;
    s_state.sequence_active = s_sequence.valid;
    s_state.sequence_index = s_sequence.index;
    s_state.sequence_count = s_sequence.count;

    const esp_err_t apply_ret = actuator_control_apply(
        output_a, output_b, servo_output, servo_enabled);
    if (apply_ret != ESP_OK) {
        ESP_LOGE(TAG, "actuator apply failed: %s; locking motion",
                 esp_err_to_name(apply_ret));
        mark_fault_locked();
        xSemaphoreGive(s_mutex);
        actuator_control_stop_all(true);
        return;
    }

    maybe_log_servo_diagnostics(
        current_ms, has_command,
        has_servo_command ? selected_servo.source : MOTION_SOURCE_NONE,
        servo_target, servo_output, servo_enabled);
    maybe_log_motor_diagnostics(
        current_ms, has_command,
        has_command ? selected.source : MOTION_SOURCE_NONE, requested, &mix,
        motor_min_percent, target_a, target_b, output_a, output_b);

    s_state.failsafe_active = false;
    if (has_command && s_sequence.valid &&
        selected.source == s_sequence.source &&
        selected.source != MOTION_SOURCE_NONE) {
        s_sequence.elapsed_ms += MOTION_MANAGER_PERIOD_MS;
        while (s_sequence.valid &&
               s_sequence.elapsed_ms >=
                   s_sequence.segments[s_sequence.index].duration_ms) {
            s_sequence.elapsed_ms -=
                s_sequence.segments[s_sequence.index].duration_ms;
            s_sequence.index++;
            if (s_sequence.index >= s_sequence.count) {
                memset(&s_sequence, 0, sizeof(s_sequence));
            }
        }
    }
    s_state.sequence_active = s_sequence.valid;
    s_state.sequence_index = s_sequence.index;
    s_state.sequence_count = s_sequence.count;
    xSemaphoreGive(s_mutex);
}

static void manager_task(void *arg)
{
    (void)arg;
    TickType_t last_wake = xTaskGetTickCount();
    while (s_running) {
        manager_step();
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(MOTION_MANAGER_PERIOD_MS));
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t motion_manager_init(void)
{
    if (s_mutex != NULL && s_running) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
        if (s_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
        memset(s_requests, 0, sizeof(s_requests));
        memset(&s_sequence, 0, sizeof(s_sequence));
        memset(&s_state, 0, sizeof(s_state));
        s_state.pose = (motion_pose_t){15.0f, 15.0f, 0.0f};
        s_state.current_servo_pulse_us = MOTION_DEFAULT_SERVO_PULSE_US;
        s_state.active_source = MOTION_SOURCE_NONE;
        s_state.available = false;
        s_faulted = false;
        s_current_motor_a = 0.0f;
        s_current_motor_b = 0.0f;
        s_current_servo_pulse_us = MOTION_DEFAULT_SERVO_PULSE_US;
        s_reverse_wait_a = 0;
        s_reverse_wait_b = 0;
        s_servo_diag_initialized = false;
        s_servo_diag_source = MOTION_SOURCE_NONE;
        s_servo_diag_target_us = MOTION_DEFAULT_SERVO_PULSE_US;
        s_servo_diag_output_us = MOTION_DEFAULT_SERVO_PULSE_US;
        s_servo_diag_enabled = false;
        s_servo_diag_last_ms = 0;
        s_motor_diag_initialized = false;
        s_motor_diag_source = MOTION_SOURCE_NONE;
        s_motor_diag_has_command = false;
        s_motor_diag_target_a = 0;
        s_motor_diag_target_b = 0;
        s_motor_diag_output_a = 0;
        s_motor_diag_output_b = 0;
        s_motor_diag_reverse_wait_a = 0;
        s_motor_diag_reverse_wait_b = 0;
        s_motor_diag_last_ms = 0;
        xSemaphoreGive(s_mutex);
    }

    const esp_err_t actuator_ret = actuator_control_init(NULL, NULL);
    if (actuator_ret != ESP_OK) {
        ESP_LOGE(TAG, "actuator initialization failed: %s",
                 esp_err_to_name(actuator_ret));
        return actuator_ret;
    }

    s_available = true;
    s_running = true;
    if (xTaskCreate(manager_task, "motion_manager", 4096, NULL, 6,
                    &s_task) != pdPASS) {
        s_running = false;
        s_available = false;
        actuator_control_stop_all(true);
        return ESP_ERR_NO_MEM;
    }
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
        s_state.available = true;
        xSemaphoreGive(s_mutex);
    }
    ESP_LOGI(TAG, "motion manager ready: period=%dms software boundary=off",
             MOTION_MANAGER_PERIOD_MS);
    return ESP_OK;
}

void motion_manager_stop(void)
{
    if (s_mutex == NULL) {
        return;
    }

    TaskHandle_t task = NULL;
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
        s_running = false;
        s_available = false;
        clear_all_requests_locked();
        s_state.available = false;
        s_state.current_motor_a_percent = 0;
        s_state.current_motor_b_percent = 0;
        s_state.current_servo_pulse_us = (int)lroundf(
            s_current_servo_pulse_us);
        s_state.servo_enabled = false;
        s_state.target_motor_a_percent = 0;
        s_state.target_motor_b_percent = 0;
        s_state.active_source = MOTION_SOURCE_NONE;
        task = s_task;
        xSemaphoreGive(s_mutex);
    }
    actuator_control_stop_all(false);
    if (task != NULL && task != xTaskGetCurrentTaskHandle()) {
        vTaskDelete(task);
        s_task = NULL;
    }
}

esp_err_t motion_manager_submit(motion_source_t source,
                                const motion_request_t *request)
{
    if (!source_valid(source) || request == NULL || request->ttl_ms == 0 ||
        request->linear < -1.0f || request->linear > 1.0f ||
        request->angular < -1.0f || request->angular > 1.0f ||
        request->ramp_ms > MOTION_MAX_SEGMENT_MS ||
        request->servo_pulse_us < ACTUATOR_SERVO_PULSE_MIN_US ||
        request->servo_pulse_us > ACTUATOR_SERVO_PULSE_MAX_US) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_mutex == NULL || !s_available || s_faulted) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    s_requests[source].valid = true;
    s_requests[source].request = *request;
    s_requests[source].expires_at_ms = now_ms() + request->ttl_ms;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

void motion_manager_clear_source(motion_source_t source)
{
    if (!source_valid(source) || s_mutex == NULL) {
        return;
    }
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
        s_requests[source].valid = false;
        if (s_sequence.valid && s_sequence.source == source) {
            memset(&s_sequence, 0, sizeof(s_sequence));
        }
        xSemaphoreGive(s_mutex);
    }
}

esp_err_t motion_manager_start_sequence(motion_source_t source,
                                         const motion_segment_t *segments,
                                         size_t count)
{
    if (!source_valid(source) || segments == NULL || count == 0 ||
        count > MOTION_MAX_SEGMENTS || s_mutex == NULL ||
        !s_available || s_faulted) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < count; ++i) {
        if (segments[i].duration_ms == 0 ||
            segments[i].duration_ms > MOTION_MAX_SEGMENT_MS ||
            segments[i].linear < -1.0f || segments[i].linear > 1.0f ||
            segments[i].angular < -1.0f || segments[i].angular > 1.0f ||
            segments[i].ramp_ms > MOTION_MAX_SEGMENT_MS ||
            segments[i].servo_pulse_us < ACTUATOR_SERVO_PULSE_MIN_US ||
            segments[i].servo_pulse_us > ACTUATOR_SERVO_PULSE_MAX_US) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_sequence.valid && source < s_sequence.source) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    memset(&s_sequence, 0, sizeof(s_sequence));
    memcpy(s_sequence.segments, segments, count * sizeof(segments[0]));
    s_sequence.source = source;
    s_sequence.count = count;
    s_sequence.valid = true;
    s_requests[source].valid = false;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

void motion_manager_stop_all(void)
{
    int held_servo_pulse_us = MOTION_DEFAULT_SERVO_PULSE_US;
    if (s_mutex != NULL && xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
        clear_all_requests_locked();
        s_current_motor_a = 0.0f;
        s_current_motor_b = 0.0f;
        s_reverse_wait_a = 0;
        s_reverse_wait_b = 0;
        s_state.current_motor_a_percent = 0;
        s_state.current_motor_b_percent = 0;
        /* Keep the last calibrated pulse for telemetry and the next follow
         * session, but release the PWM ownership when all motion stops. This
         * leaves the head at its current angle without locking the servo. */
        held_servo_pulse_us = (int)lroundf(clampf(
            s_current_servo_pulse_us,
            (float)ACTUATOR_SERVO_PULSE_MIN_US,
            (float)ACTUATOR_SERVO_PULSE_MAX_US));
        s_state.current_servo_pulse_us = held_servo_pulse_us;
        s_state.servo_enabled = false;
        s_state.target_motor_a_percent = 0;
        s_state.target_motor_b_percent = 0;
        s_state.active_source = MOTION_SOURCE_NONE;
        s_state.sequence_active = false;
        s_state.sequence_index = 0;
        s_state.sequence_count = 0;
        xSemaphoreGive(s_mutex);
    }
    actuator_control_stop_all(false);
    ESP_LOGI(TAG, "all motion stopped: servo PWM released at pulse_us=%d",
             held_servo_pulse_us);
}

void motion_manager_reset_pose(void)
{
    if (s_mutex == NULL || xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    s_state.pose = (motion_pose_t){15.0f, 15.0f, 0.0f};
    s_state.boundary_limited = false;
    xSemaphoreGive(s_mutex);
}

void motion_manager_get_state(motion_state_t *out_state)
{
    if (out_state == NULL) {
        return;
    }
    if (s_mutex == NULL || xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        memset(out_state, 0, sizeof(*out_state));
        out_state->active_source = MOTION_SOURCE_NONE;
        out_state->current_servo_pulse_us = MOTION_DEFAULT_SERVO_PULSE_US;
        return;
    }
    *out_state = s_state;
    xSemaphoreGive(s_mutex);
}
