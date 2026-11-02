#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "face_target_tracker.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FACE_FOLLOW_SERVO_PULSE_MIN_US 1100
#define FACE_FOLLOW_SERVO_PULSE_CENTER_US 1500
#define FACE_FOLLOW_SERVO_PULSE_MAX_US 1900

typedef struct {
    float integral;
    float previous_error;
    float derivative;
    uint32_t previous_ms;
    bool initialized;
} face_follow_pid_state_t;

typedef enum {
    FACE_FOLLOW_STOP_NONE = 0,
    FACE_FOLLOW_STOP_INVALID_TARGET,
    FACE_FOLLOW_STOP_STALE_TARGET,
    FACE_FOLLOW_STOP_TARGET_SIZE,
    FACE_FOLLOW_STOP_TOO_CLOSE,
    FACE_FOLLOW_STOP_PREDICTED_TARGET,
    FACE_FOLLOW_STOP_DIRECTION_BRAKE,
    FACE_FOLLOW_STOP_FAR_TARGET,
} face_follow_stop_reason_t;

typedef struct {
    int frame_width;
    int frame_height;
    /* Desired face height retained for distance-control tuning. */
    float target_height_ratio;
    /* Measured face height below this threshold permits slow forward motion. */
    float far_height_ratio;
    /* Vertical tracking window. The camera pitch is considered centered once
     * the face box stays inside y1..y2. */
    float target_box_y1;
    float target_box_y2;
    /* Horizontal stop band, as a fraction of the frame half-width. The yaw
     * loop works from the face *centre*, not from the box edges: the old
     * box-edge error only reacted once a face edge crossed x=80/240, which
     * for a typical 80 px face left a +-40 px band with no correction at all
     * and made the response depend on how large the face was. */
    float yaw_deadband_ratio;
    /* Smaller band that has to be reached before an active correction ends.
     * The gap between the two bands is the hysteresis that keeps one or two
     * pixels of detector jitter from starting and stopping the wheels on
     * alternate frames. */
    float yaw_release_ratio;
    /* Fixed yaw command used while a correction is active. The wheel pair
     * maps this to 28% + u*72% duty and only breaks away at roughly 34%, so a
     * proportional command leaves small corrections stalled right on the
     * friction threshold. The magnitude is therefore fixed well above
     * breakaway and the dose is carried by the pulse width alone. */
    float yaw_drive;
    /* Lower command used when the box is extrapolated rather than measured. */
    float yaw_predicted_drive;
    /* Shortest yaw pulse. The wheels need this long just to ramp past the
     * breakaway duty before the pulse is taken away. */
    uint32_t yaw_pulse_ms;
    /* Extra pulse time per pixel of centre offset, and its ceiling. */
    float yaw_pulse_ms_per_px;
    uint32_t yaw_pulse_max_ms;
    /* Dose multiplier applied when the face is to screen-right, where the
     * chassis turns harder than toward screen-left. */
    float yaw_right_pulse_scale;
    float vertical_kp;
    float vertical_ki;
    float vertical_kd;
    float vertical_integral_limit;
    float max_servo_correction_us;
    /* The face must cover this box for a confirmed too-close state. */
    float near_box_x1;
    float near_box_y1;
    float near_box_x2;
    float near_box_y2;
    /* Release geometry and size threshold provide near-state hysteresis. */
    float near_release_box_x1;
    float near_release_box_y1;
    float near_release_box_x2;
    float near_release_box_y2;
    /* Size fallback handles a close face clipped by a screen edge. */
    float near_height_ratio;
    float near_release_height_ratio;
    uint8_t near_confirm_frames;
    uint8_t near_release_frames;
} face_follow_controller_config_t;

typedef struct {
    float linear;
    float angular;
    face_follow_stop_reason_t stop_reason;
    int servo_pulse_us;
    bool servo_enabled;
    /* Milliseconds the angular part stays applied after the command was
     * issued; 0 keeps it for the whole detector interval. The publisher of
     * the command drops the angular part once this window has elapsed. */
    uint32_t turn_pulse_ms;
} face_follow_command_t;

typedef struct {
    face_follow_controller_config_t config;
    face_follow_pid_state_t vertical_pid;
    /* Yaw is a pulsed, fixed-amplitude controller; see yaw_drive. */
    bool yaw_engaged;
    uint32_t yaw_pulse_ms;
    int servo_pulse_us;
    bool has_servo_pulse;
    bool too_close_active;
    uint8_t too_close_enter_streak;
    uint8_t too_close_release_streak;
} face_follow_controller_t;

void face_follow_controller_config_init(
    face_follow_controller_config_t *config);

void face_follow_controller_init(
    face_follow_controller_t *controller,
    const face_follow_controller_config_t *config);

void face_follow_controller_reset(face_follow_controller_t *controller);

/* Seed tracking from the last actuator position instead of assuming 1500 us. */
void face_follow_controller_set_servo_pulse(
    face_follow_controller_t *controller, int servo_pulse_us);

face_follow_command_t face_follow_controller_step(
    face_follow_controller_t *controller,
    const face_target_t *target,
    bool target_fresh,
    uint32_t now_ms);

#ifdef __cplusplus
}
#endif
