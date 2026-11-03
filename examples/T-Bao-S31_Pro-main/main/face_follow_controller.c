#include "face_follow_controller.h"

#include <math.h>

#define FACE_FOLLOW_DEFAULT_WIDTH 320
#define FACE_FOLLOW_DEFAULT_HEIGHT 320
#define FACE_FOLLOW_DEFAULT_TARGET_HEIGHT 0.44f
#define FACE_FOLLOW_DEFAULT_FAR_HEIGHT 0.40f
#define FACE_FOLLOW_DEFAULT_TARGET_BOX_Y1 70.0f
#define FACE_FOLLOW_DEFAULT_TARGET_BOX_Y2 250.0f
/* 水平小车转向：定幅脉冲控制
 *
 * motion_manager maps the yaw command to wheel duty as 28% + u * 72%, and the
 * gearbox only breaks away at roughly 34% duty. Measured on hardware, a face
 * parked 60-80 px off centre with u = 0.055 (29% duty) never moved at all,
 * while u = 0.09 (34.5%) already turned the chassis far enough to overshoot a
 * whole detector interval. Proportional amplitude control therefore has no
 * usable range: it either stalls on the friction threshold or slams.
 *
 * The loop is therefore a fixed-amplitude pulsed controller. While a
 * correction is active the wheels are driven at one working point well above
 * breakaway, and the *dose* - how long that drive stays on - is set by the
 * distance the face has to travel. The brake ramp finishes the movement, and
 * the next detector frame decides whether another pulse is needed.
 */
#define FACE_FOLLOW_DEFAULT_YAW_DRIVE 0.14f
#define FACE_FOLLOW_DEFAULT_YAW_PREDICTED_DRIVE 0.12f
/* Stop band / release band as a fraction of the frame half-width.
 *
 * One minimum wheel pulse moves the face roughly 20-30 px, and that is the
 * finest correction this chassis can make. The release band has to be at
 * least that wide, otherwise a pulse started at the stop-band edge lands just
 * outside the release band on the other side and the controller nudges back
 * and forth for many frames before it happens to stop inside it. 0.20 / 0.14
 * of 160 px is 32 px / 22 px. */
#define FACE_FOLLOW_DEFAULT_YAW_DEADBAND_RATIO 0.20f
#define FACE_FOLLOW_DEFAULT_YAW_RELEASE_RATIO 0.14f
#define FACE_FOLLOW_DEFAULT_YAW_PULSE_MS 40U
#define FACE_FOLLOW_DEFAULT_YAW_PULSE_MS_PER_PX 0.7f
#define FACE_FOLLOW_DEFAULT_YAW_PULSE_MAX_MS 200U
/* This chassis turns noticeably harder toward screen-right than toward the
 * screen-left (the original controller compensated with a 0.05/0.09 command
 * cap). A right-edge correction therefore overshot, sometimes far enough to
 * throw the face to the left edge, and the resulting left/right exchange
 * could walk the face out of frame. Scale the right-hand dose down instead of
 * lowering its drive, so the wheel duty stays above the gearbox breakaway. */
#define FACE_FOLLOW_DEFAULT_YAW_RIGHT_PULSE_SCALE 0.60f
// 垂直舵机 PID
#define FACE_FOLLOW_DEFAULT_VERTICAL_KP 360.0f
#define FACE_FOLLOW_DEFAULT_VERTICAL_KI 20.0f
#define FACE_FOLLOW_DEFAULT_VERTICAL_KD 4.0f
#define FACE_FOLLOW_DEFAULT_VERTICAL_INTEGRAL_LIMIT 0.35f
/* The vertical PID output is a pulse-rate correction in us/s. */
#define FACE_FOLLOW_DEFAULT_MAX_SERVO_CORRECTION_US 480.0f
#define FACE_FOLLOW_DEFAULT_CONTROL_PERIOD_S 0.035f
#define FACE_FOLLOW_DEFAULT_NEAR_BOX_X1 120.0f
#define FACE_FOLLOW_DEFAULT_NEAR_BOX_Y1 115.0f
#define FACE_FOLLOW_DEFAULT_NEAR_BOX_X2 200.0f
#define FACE_FOLLOW_DEFAULT_NEAR_BOX_Y2 205.0f
#define FACE_FOLLOW_DEFAULT_NEAR_RELEASE_BOX_X1 120.0f
#define FACE_FOLLOW_DEFAULT_NEAR_RELEASE_BOX_Y1 115.0f
#define FACE_FOLLOW_DEFAULT_NEAR_RELEASE_BOX_X2 200.0f
#define FACE_FOLLOW_DEFAULT_NEAR_RELEASE_BOX_Y2 205.0f
#define FACE_FOLLOW_DEFAULT_NEAR_HEIGHT 0.58f
#define FACE_FOLLOW_DEFAULT_NEAR_RELEASE_HEIGHT 0.54f
#define FACE_FOLLOW_DEFAULT_NEAR_CONFIRM_FRAMES 2U
#define FACE_FOLLOW_DEFAULT_NEAR_RELEASE_FRAMES 2U
#define FACE_FOLLOW_PID_DERIVATIVE_LIMIT 4.0f
#define FACE_FOLLOW_PID_DERIVATIVE_ALPHA 0.25f
/* On this mount a positive pulse tilts the camera toward the lower image
 * edge, so a face below center needs a negative correction. */
#define FACE_FOLLOW_SERVO_Y_PULSE_SIGN -1.0f

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

static float target_box_axis_error(float leading_edge,
                                   float trailing_edge,
                                   float target_min,
                                   float target_max,
                                   float half_frame)
{
    if (half_frame <= 0.0f) {
        return 0.0f;
    }

    const bool outside_min = leading_edge < target_min;
    const bool outside_max = trailing_edge > target_max;
    if (outside_min && outside_max) {
        /* A large box can cross both sides while still being centered. In
         * that case use its center only to choose the correction direction. */
        const float face_center = (leading_edge + trailing_edge) * 0.5f;
        const float target_center = (target_min + target_max) * 0.5f;
        return (face_center - target_center) / half_frame;
    }
    if (outside_min) {
        return (leading_edge - target_min) / half_frame;
    }
    if (outside_max) {
        return (trailing_edge - target_max) / half_frame;
    }
    return 0.0f;
}

static int clamp_servo_pulse(int pulse_us)
{
    return (int)lroundf(clampf((float)pulse_us,
                               (float)FACE_FOLLOW_SERVO_PULSE_MIN_US,
                               (float)FACE_FOLLOW_SERVO_PULSE_MAX_US));
}

static void pid_reset(face_follow_pid_state_t *pid)
{
    if (pid != NULL) {
        *pid = (face_follow_pid_state_t){0};
    }
}

static float pid_step(face_follow_pid_state_t *pid,
                      float error,
                      uint32_t now_ms,
                      float kp,
                      float ki,
                      float kd,
                      float integral_limit,
                      float output_min,
                      float output_max,
                      bool allow_integral)
{
    if (pid == NULL) {
        return 0.0f;
    }

    float dt_s = 0.0f;
    if (!pid->initialized) {
        pid->initialized = true;
        pid->previous_error = error;
        pid->previous_ms = now_ms;
        pid->derivative = 0.0f;
    } else {
        dt_s = (float)(now_ms - pid->previous_ms) / 1000.0f;
        dt_s = clampf(dt_s, 0.02f, 0.5f);
        const float raw_derivative = clampf(
            (error - pid->previous_error) / dt_s,
            -FACE_FOLLOW_PID_DERIVATIVE_LIMIT,
            FACE_FOLLOW_PID_DERIVATIVE_LIMIT);
        pid->derivative += FACE_FOLLOW_PID_DERIVATIVE_ALPHA *
                           (raw_derivative - pid->derivative);
        pid->previous_error = error;
        pid->previous_ms = now_ms;
    }

    const float candidate_integral =
        allow_integral && dt_s > 0.0f
            ? clampf(pid->integral + error * dt_s, -integral_limit,
                     integral_limit)
            : pid->integral;
    const float candidate_output = kp * error +
                                   ki * candidate_integral +
                                   kd * pid->derivative;
    const bool saturated_high = candidate_output > output_max;
    const bool saturated_low = candidate_output < output_min;
    const bool drives_out_of_saturation =
        (saturated_high && error < 0.0f) ||
        (saturated_low && error > 0.0f);
    if (allow_integral &&
        ((!saturated_high && !saturated_low) ||
         drives_out_of_saturation)) {
        pid->integral = candidate_integral;
    }

    return clampf(kp * error + ki * pid->integral + kd * pid->derivative,
                  output_min, output_max);
}

static face_follow_command_t stop_command(
    const face_follow_controller_t *controller,
    face_follow_stop_reason_t reason)
{
    const bool hold_servo = controller != NULL && controller->has_servo_pulse;
    const int servo_pulse = hold_servo
                                ? clamp_servo_pulse(controller->servo_pulse_us)
                                : FACE_FOLLOW_SERVO_PULSE_CENTER_US;
    return (face_follow_command_t){
        .linear = 0.0f,
        .angular = 0.0f,
        .stop_reason = reason,
        .servo_pulse_us = servo_pulse,
        .servo_enabled = hold_servo,
    };
}

static bool box_covers(const face_target_t *target,
                       float x1,
                       float y1,
                       float x2,
                       float y2)
{
    return target != NULL && target->x1 <= x1 && target->y1 <= y1 &&
           target->x2 >= x2 && target->y2 >= y2;
}

static bool update_too_close_state(face_follow_controller_t *controller,
                                   const face_target_t *target)
{
    if (controller == NULL || target == NULL || target->predicted) {
        return controller != NULL && controller->too_close_active;
    }

    const face_follow_controller_config_t *config = &controller->config;
    const bool covers_near_box =
        box_covers(target, config->near_box_x1, config->near_box_y1,
                   config->near_box_x2, config->near_box_y2);
    const bool face_is_near =
        target->height_ratio >= config->near_height_ratio;

    if (!controller->too_close_active) {
        controller->too_close_release_streak = 0;
        if (!covers_near_box && !face_is_near) {
            controller->too_close_enter_streak = 0;
            return false;
        }

        if (controller->too_close_enter_streak < UINT8_MAX) {
            controller->too_close_enter_streak++;
        }
        if (controller->too_close_enter_streak >=
            config->near_confirm_frames) {
            controller->too_close_active = true;
            controller->too_close_enter_streak = 0;
        }
        return controller->too_close_active;
    }

    const bool covers_release_box = box_covers(
        target, config->near_release_box_x1, config->near_release_box_y1,
        config->near_release_box_x2, config->near_release_box_y2);
    const bool face_is_still_near =
        target->height_ratio >= config->near_release_height_ratio;
    if (covers_release_box || face_is_still_near) {
        controller->too_close_release_streak = 0;
        return true;
    }

    if (controller->too_close_release_streak < UINT8_MAX) {
        controller->too_close_release_streak++;
    }
    if (controller->too_close_release_streak >=
        config->near_release_frames) {
        controller->too_close_active = false;
        controller->too_close_release_streak = 0;
        controller->too_close_enter_streak = 0;
    }
    return controller->too_close_active;
}

static int servo_pulse_for_target(face_follow_controller_t *controller,
                                  const face_target_t *target,
                                  uint32_t now_ms)
{
    if (controller == NULL || target == NULL) {
        return FACE_FOLLOW_SERVO_PULSE_CENTER_US;
    }

    if (!controller->has_servo_pulse) {
        controller->servo_pulse_us = FACE_FOLLOW_SERVO_PULSE_CENTER_US;
        controller->has_servo_pulse = true;
    }
    const int current_pulse = clamp_servo_pulse(controller->servo_pulse_us);
    controller->servo_pulse_us = current_pulse;

    if (target->predicted) {
        return current_pulse;
    }

    const float half_height = (float)controller->config.frame_height * 0.5f;
    const float vertical_error = target_box_axis_error(
        target->y1, target->y2, controller->config.target_box_y1,
        controller->config.target_box_y2, half_height);
    if (vertical_error == 0.0f) {
        /* A centered face means the current camera angle is already correct.
         * Reset only the PID memory so detector noise cannot drift the head. */
        pid_reset(&controller->vertical_pid);
        return current_pulse;
    }

    const bool had_previous_sample = controller->vertical_pid.initialized;
    const uint32_t previous_ms = controller->vertical_pid.previous_ms;
    const float correction = pid_step(
        &controller->vertical_pid, vertical_error, now_ms,
        controller->config.vertical_kp, controller->config.vertical_ki,
        controller->config.vertical_kd,
        controller->config.vertical_integral_limit,
        -controller->config.max_servo_correction_us,
        controller->config.max_servo_correction_us, true);

    /* This is an incremental position controller. The pulse starts from the
     * last commanded position and the PID output is integrated over the
     * sample interval, so reacquisition never jumps to the mechanical center.
     * The fallback period is only used for the first sample. */
    float dt_s = FACE_FOLLOW_DEFAULT_CONTROL_PERIOD_S;
    if (had_previous_sample) {
        dt_s = clampf((float)(now_ms - previous_ms) / 1000.0f,
                      0.02f, 0.5f);
    }
    const float requested_pulse =
        (float)current_pulse +
        FACE_FOLLOW_SERVO_Y_PULSE_SIGN * correction * dt_s;
    const int pulse = clamp_servo_pulse((int)lroundf(requested_pulse));
    controller->servo_pulse_us = pulse;
    controller->has_servo_pulse = true;
    return pulse;
}

static float horizontal_error_for_target(
    const face_follow_controller_t *controller, const face_target_t *target)
{
    if (controller == NULL || target == NULL) {
        return 0.0f;
    }

    const float half_width = (float)controller->config.frame_width * 0.5f;
    if (half_width <= 0.0f) {
        return 0.0f;
    }
    /* Centre-based error: positive when the face is to screen-right. */
    return (target->center_x - half_width) / half_width;
}

static float horizontal_command_for_target(
    face_follow_controller_t *controller,
    const face_target_t *target,
    uint32_t now_ms)
{
    (void)now_ms;
    if (controller == NULL || target == NULL) {
        return 0.0f;
    }

    const face_follow_controller_config_t *config = &controller->config;
    const float half_width = (float)config->frame_width * 0.5f;
    const float offset_px = target->center_x - half_width;
    const float abs_px = fabsf(offset_px);

    controller->yaw_pulse_ms = 0;

    /* Hysteresis: start correcting once the face centre leaves the stop band
     * and keep correcting until it is back inside the smaller release band.
     * Without the gap, detector jitter at the band edge made the wheels start
     * and stop on alternate frames, which reads as left/right chatter. */
    bool engaged = controller->yaw_engaged;
    if (engaged) {
        if (abs_px <= config->yaw_release_ratio * half_width) {
            engaged = false;
        }
    } else if (abs_px > config->yaw_deadband_ratio * half_width) {
        engaged = true;
    }
    controller->yaw_engaged = engaged;
    if (!engaged) {
        return 0.0f;
    }

    /* The dose is the pulse width, not the command magnitude. The pulse grows
     * with the distance the face has to travel so a face at the image border
     * gets a long correction while one just outside the stop band gets the
     * shortest kick the gearbox can still act on. */
    float dose_ms = abs_px * config->yaw_pulse_ms_per_px;
    if (offset_px > 0.0f) {
        /* Face on screen-right: this direction turns faster, so give it a
         * proportionally shorter dose. The floor still applies, which keeps
         * the pulse long enough for the gearbox to break away. */
        dose_ms *= config->yaw_right_pulse_scale;
    }
    const uint32_t pulse_ms =
        (uint32_t)lroundf(clampf(dose_ms, (float)config->yaw_pulse_ms,
                                 (float)config->yaw_pulse_max_ms));
    controller->yaw_pulse_ms = pulse_ms;

    const float drive =
        target->predicted ? config->yaw_predicted_drive : config->yaw_drive;
    /* Positive offset means the face is to screen-right; the negative angular
     * command turns the chassis that way. */
    return offset_px > 0.0f ? -drive : drive;
}

static bool valid_config(const face_follow_controller_config_t *config)
{
    return config != NULL && config->frame_width > 0 &&
           config->frame_height > 0 &&
           isfinite(config->target_height_ratio) &&
           config->target_height_ratio > 0.0f &&
           config->target_height_ratio <= 1.0f &&
           isfinite(config->far_height_ratio) &&
           config->far_height_ratio > 0.0f &&
           config->far_height_ratio < config->target_height_ratio &&
           isfinite(config->target_box_y1) &&
           isfinite(config->target_box_y2) &&
           config->target_box_y1 >= 0.0f &&
           config->target_box_y2 <= (float)config->frame_height &&
           config->target_box_y1 < config->target_box_y2 &&
           isfinite(config->near_height_ratio) &&
           config->near_height_ratio > 0.0f &&
           config->near_height_ratio <= 1.0f &&
           isfinite(config->near_release_height_ratio) &&
           config->near_release_height_ratio > 0.0f &&
           config->near_release_height_ratio <= config->near_height_ratio &&
           config->far_height_ratio < config->near_release_height_ratio &&
           isfinite(config->yaw_deadband_ratio) &&
           config->yaw_deadband_ratio > 0.0f &&
           config->yaw_deadband_ratio < 1.0f &&
           isfinite(config->yaw_release_ratio) &&
           config->yaw_release_ratio >= 0.0f &&
           config->yaw_release_ratio < config->yaw_deadband_ratio &&
           isfinite(config->yaw_drive) && config->yaw_drive > 0.0f &&
           config->yaw_drive <= 1.0f &&
           isfinite(config->yaw_predicted_drive) &&
           config->yaw_predicted_drive > 0.0f &&
           config->yaw_predicted_drive <= 1.0f &&
           config->yaw_pulse_ms > 0U && config->yaw_pulse_ms <= 1000U &&
           isfinite(config->yaw_pulse_ms_per_px) &&
           config->yaw_pulse_ms_per_px > 0.0f &&
           config->yaw_pulse_max_ms >= config->yaw_pulse_ms &&
           isfinite(config->yaw_right_pulse_scale) &&
           config->yaw_right_pulse_scale > 0.0f &&
           config->yaw_right_pulse_scale <= 1.0f &&
           isfinite(config->vertical_kp) && config->vertical_kp >= 0.0f &&
           isfinite(config->vertical_ki) && config->vertical_ki >= 0.0f &&
           isfinite(config->vertical_kd) && config->vertical_kd >= 0.0f &&
           isfinite(config->vertical_integral_limit) &&
           config->vertical_integral_limit >= 0.0f &&
           isfinite(config->max_servo_correction_us) &&
           config->max_servo_correction_us > 0.0f &&
           config->max_servo_correction_us <=
               (float)(FACE_FOLLOW_SERVO_PULSE_MAX_US -
                       FACE_FOLLOW_SERVO_PULSE_MIN_US) &&
           isfinite(config->near_box_x1) && isfinite(config->near_box_y1) &&
           isfinite(config->near_box_x2) && isfinite(config->near_box_y2) &&
           isfinite(config->near_release_box_x1) &&
           isfinite(config->near_release_box_y1) &&
           isfinite(config->near_release_box_x2) &&
           isfinite(config->near_release_box_y2) &&
           config->near_box_x1 >= 0.0f && config->near_box_y1 >= 0.0f &&
           config->near_box_x2 <= (float)config->frame_width &&
           config->near_box_y2 <= (float)config->frame_height &&
           config->near_box_x1 < config->near_box_x2 &&
           config->near_box_y1 < config->near_box_y2 &&
           config->near_release_box_x1 >= 0.0f &&
           config->near_release_box_y1 >= 0.0f &&
           config->near_release_box_x2 <= (float)config->frame_width &&
           config->near_release_box_y2 <= (float)config->frame_height &&
           config->near_release_box_x1 < config->near_release_box_x2 &&
           config->near_release_box_y1 < config->near_release_box_y2 &&
           config->near_release_box_x1 >= config->near_box_x1 &&
           config->near_release_box_y1 >= config->near_box_y1 &&
           config->near_release_box_x2 <= config->near_box_x2 &&
           config->near_release_box_y2 <= config->near_box_y2 &&
           config->near_confirm_frames > 0 &&
           config->near_release_frames > 0;
}

void face_follow_controller_config_init(
    face_follow_controller_config_t *config)
{
    if (config == NULL) {
        return;
    }
    *config = (face_follow_controller_config_t){
        .frame_width = FACE_FOLLOW_DEFAULT_WIDTH,
        .frame_height = FACE_FOLLOW_DEFAULT_HEIGHT,
        .target_height_ratio = FACE_FOLLOW_DEFAULT_TARGET_HEIGHT,
        .far_height_ratio = FACE_FOLLOW_DEFAULT_FAR_HEIGHT,
        .target_box_y1 = FACE_FOLLOW_DEFAULT_TARGET_BOX_Y1,
        .target_box_y2 = FACE_FOLLOW_DEFAULT_TARGET_BOX_Y2,
        .yaw_deadband_ratio = FACE_FOLLOW_DEFAULT_YAW_DEADBAND_RATIO,
        .yaw_release_ratio = FACE_FOLLOW_DEFAULT_YAW_RELEASE_RATIO,
        .yaw_drive = FACE_FOLLOW_DEFAULT_YAW_DRIVE,
        .yaw_predicted_drive = FACE_FOLLOW_DEFAULT_YAW_PREDICTED_DRIVE,
        .yaw_pulse_ms = FACE_FOLLOW_DEFAULT_YAW_PULSE_MS,
        .yaw_pulse_ms_per_px = FACE_FOLLOW_DEFAULT_YAW_PULSE_MS_PER_PX,
        .yaw_pulse_max_ms = FACE_FOLLOW_DEFAULT_YAW_PULSE_MAX_MS,
        .yaw_right_pulse_scale = FACE_FOLLOW_DEFAULT_YAW_RIGHT_PULSE_SCALE,
        .vertical_kp = FACE_FOLLOW_DEFAULT_VERTICAL_KP,
        .vertical_ki = FACE_FOLLOW_DEFAULT_VERTICAL_KI,
        .vertical_kd = FACE_FOLLOW_DEFAULT_VERTICAL_KD,
        .vertical_integral_limit = FACE_FOLLOW_DEFAULT_VERTICAL_INTEGRAL_LIMIT,
        .max_servo_correction_us = FACE_FOLLOW_DEFAULT_MAX_SERVO_CORRECTION_US,
        .near_box_x1 = FACE_FOLLOW_DEFAULT_NEAR_BOX_X1,
        .near_box_y1 = FACE_FOLLOW_DEFAULT_NEAR_BOX_Y1,
        .near_box_x2 = FACE_FOLLOW_DEFAULT_NEAR_BOX_X2,
        .near_box_y2 = FACE_FOLLOW_DEFAULT_NEAR_BOX_Y2,
        .near_release_box_x1 = FACE_FOLLOW_DEFAULT_NEAR_RELEASE_BOX_X1,
        .near_release_box_y1 = FACE_FOLLOW_DEFAULT_NEAR_RELEASE_BOX_Y1,
        .near_release_box_x2 = FACE_FOLLOW_DEFAULT_NEAR_RELEASE_BOX_X2,
        .near_release_box_y2 = FACE_FOLLOW_DEFAULT_NEAR_RELEASE_BOX_Y2,
        .near_height_ratio = FACE_FOLLOW_DEFAULT_NEAR_HEIGHT,
        .near_release_height_ratio = FACE_FOLLOW_DEFAULT_NEAR_RELEASE_HEIGHT,
        .near_confirm_frames = FACE_FOLLOW_DEFAULT_NEAR_CONFIRM_FRAMES,
        .near_release_frames = FACE_FOLLOW_DEFAULT_NEAR_RELEASE_FRAMES,
    };
}

void face_follow_controller_init(
    face_follow_controller_t *controller,
    const face_follow_controller_config_t *config)
{
    if (controller == NULL) {
        return;
    }
    face_follow_controller_config_t defaults;
    face_follow_controller_config_init(&defaults);
    controller->config = valid_config(config) ? *config : defaults;
    controller->servo_pulse_us = FACE_FOLLOW_SERVO_PULSE_CENTER_US;
    controller->has_servo_pulse = false;
    controller->too_close_active = false;
    controller->too_close_enter_streak = 0;
    controller->too_close_release_streak = 0;
    face_follow_controller_reset(controller);
}

void face_follow_controller_reset(face_follow_controller_t *controller)
{
    if (controller == NULL) {
        return;
    }
    pid_reset(&controller->vertical_pid);
    controller->yaw_engaged = false;
    controller->yaw_pulse_ms = 0;
    controller->too_close_active = false;
    controller->too_close_enter_streak = 0;
    controller->too_close_release_streak = 0;
}

void face_follow_controller_set_servo_pulse(
    face_follow_controller_t *controller, int servo_pulse_us)
{
    if (controller == NULL) {
        return;
    }
    controller->servo_pulse_us = clamp_servo_pulse(servo_pulse_us);
    controller->has_servo_pulse = true;
    pid_reset(&controller->vertical_pid);
}

face_follow_command_t face_follow_controller_step(
    face_follow_controller_t *controller,
    const face_target_t *target,
    bool target_fresh,
    uint32_t now_ms)
{
    if (controller == NULL || target == NULL || !target->valid) {
        if (controller != NULL) {
            face_follow_controller_reset(controller);
        }
        return stop_command(controller, FACE_FOLLOW_STOP_INVALID_TARGET);
    }
    if (!target_fresh) {
        face_follow_controller_reset(controller);
        return stop_command(controller, FACE_FOLLOW_STOP_STALE_TARGET);
    }
    if (!isfinite(target->x1) || !isfinite(target->y1) ||
        !isfinite(target->x2) || !isfinite(target->y2) ||
        !isfinite(target->center_x) || !isfinite(target->center_y) ||
        !isfinite(target->height_ratio) || target->height_ratio < 0.0f ||
        target->height_ratio > 1.0f) {
        face_follow_controller_reset(controller);
        return stop_command(controller, FACE_FOLLOW_STOP_INVALID_TARGET);
    }

    const float horizontal_error =
        horizontal_error_for_target(controller, target);
    /* "Centered" uses the same stop band as the yaw loop, so distance control
     * and the too-close release agree with what the wheels are doing. */
    const bool horizontal_centered =
        fabsf(horizontal_error) <= controller->config.yaw_deadband_ratio;
    const bool too_close = update_too_close_state(controller, target);
    const bool far_target = !target->predicted &&
                            target->height_ratio <
                                controller->config.far_height_ratio;
    const bool too_close_pending =
        !too_close && controller->too_close_enter_streak > 0;
    const bool near_target = !target->predicted && !far_target &&
                             (too_close || too_close_pending);
    const int servo_pulse_us =
        servo_pulse_for_target(controller, target, now_ms);
    if (near_target && !horizontal_centered) {
        /* A close face at an edge must be turned back into the frame before
         * distance control is allowed to reverse the vehicle. The camera
         * pitch remains an independent axis and continues to track y. */
        const float angular =
            horizontal_command_for_target(controller, target, now_ms);
        return (face_follow_command_t){
            .linear = 0.0f,
            .angular = angular,
            .stop_reason = FACE_FOLLOW_STOP_TOO_CLOSE,
            .servo_pulse_us = servo_pulse_us,
            .servo_enabled = true,
            .turn_pulse_ms = controller->yaw_pulse_ms,
        };
    }

    if (near_target) {
        /* A centered close face: hold still. The chassis does not back away
         * any more (that reverse mixed into the wheel pair shifted one wheel
         * onto its breakaway duty and made the turn erratic); the too-close
         * state is now reported only. Pitch tracking remains active. */
        controller->yaw_engaged = false;
        controller->yaw_pulse_ms = 0;
        return (face_follow_command_t){
            .linear = 0.0f,
            .angular = 0.0f,
            .stop_reason = FACE_FOLLOW_STOP_TOO_CLOSE,
            .servo_pulse_us = servo_pulse_us,
            .servo_enabled = true,
        };
    }

    const float angular =
        horizontal_command_for_target(controller, target, now_ms);

    /* No forward creep. The far-target advance used to be mixed into the same
     * wheel pair as the yaw pulse, and on this chassis it pulled one wheel
     * down to ~35% duty - right on the gearbox breakaway point - so the turn
     * became erratic exactly when the face was far away and the face could
     * drift out of frame. Turning alone keeps both wheels symmetric. */
    const face_follow_stop_reason_t reason =
        target->predicted ? FACE_FOLLOW_STOP_PREDICTED_TARGET
                          : FACE_FOLLOW_STOP_NONE;

    return (face_follow_command_t){
        .linear = 0.0f,
        .angular = angular,
        .stop_reason = reason,
        .servo_pulse_us = servo_pulse_us,
        .servo_enabled = true,
        .turn_pulse_ms = controller->yaw_pulse_ms,
    };
}
