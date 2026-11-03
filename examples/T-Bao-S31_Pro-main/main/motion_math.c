#include "motion_math.h"

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

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

static float positive_boundary_scale(float distance,
                                     float soft_distance,
                                     float hard_distance)
{
    if (distance <= hard_distance) {
        return 0.0f;
    }
    if (distance >= soft_distance) {
        return 1.0f;
    }
    return clampf((distance - hard_distance) /
                      (soft_distance - hard_distance),
                  0.0f, 1.0f);
}

static float min_scale_for_axis(float position,
                                float velocity,
                                float minimum,
                                float maximum,
                                const motion_bounds_t *bounds,
                                float dt_s)
{
    if (velocity > 0.0f) {
        float scale = positive_boundary_scale(maximum - position,
                                              bounds->soft_distance_cm,
                                              bounds->hard_distance_cm);
        if (dt_s > 0.0f && velocity * dt_s > maximum - position) {
            scale = fminf(scale, (maximum - position) / (velocity * dt_s));
        }
        return clampf(scale, 0.0f, 1.0f);
    }
    if (velocity < 0.0f) {
        float scale = positive_boundary_scale(position - minimum,
                                              bounds->soft_distance_cm,
                                              bounds->hard_distance_cm);
        if (dt_s > 0.0f && -velocity * dt_s > position - minimum) {
            scale = fminf(scale, (position - minimum) / (-velocity * dt_s));
        }
        return clampf(scale, 0.0f, 1.0f);
    }
    return 1.0f;
}

void motion_math_mix_wheels(float linear,
                            float angular,
                            float turn_gain,
                            float motor_a_sign,
                            float motor_b_sign,
                            motion_wheel_mix_t *out)
{
    if (out == NULL) {
        return;
    }

    const float wheel_a = clampf(linear + turn_gain * angular, -1.0f, 1.0f);
    const float wheel_b = clampf(linear - turn_gain * angular, -1.0f, 1.0f);
    out->motor_a = clampf(wheel_a * motor_a_sign, -1.0f, 1.0f);
    out->motor_b = clampf(wheel_b * motor_b_sign, -1.0f, 1.0f);
}

int motion_math_normalized_to_percent(float normalized,
                                      float minimum_percent,
                                      float maximum_percent)
{
    if (!isfinite(normalized) || !isfinite(minimum_percent) ||
        !isfinite(maximum_percent)) {
        return 0;
    }

    const float maximum = clampf(maximum_percent, 0.0f, 100.0f);
    const float minimum = clampf(minimum_percent, 0.0f, maximum);
    const float magnitude = fabsf(clampf(normalized, -1.0f, 1.0f));
    if (magnitude < 0.001f) {
        return 0;
    }

    const float percent = minimum + magnitude * (maximum - minimum);
    const int rounded = (int)(percent + 0.5f);
    return normalized < 0.0f ? -rounded : rounded;
}

float motion_math_ramp_toward(float current, float target, float max_delta)
{
    if (max_delta <= 0.0f) {
        return current;
    }
    if (current < target) {
        return fminf(current + max_delta, target);
    }
    if (current > target) {
        return fmaxf(current - max_delta, target);
    }
    return target;
}

void motion_math_limit_twist(const motion_pose_t *pose,
                             motion_twist_t requested,
                             float linear_speed_cm_s,
                             float dt_s,
                             const motion_bounds_t *bounds,
                             motion_twist_t *limited,
                             bool *boundary_limited)
{
    if (pose == NULL || bounds == NULL || limited == NULL) {
        return;
    }

    *limited = requested;
    if (boundary_limited != NULL) {
        *boundary_limited = false;
    }
    if (requested.linear == 0.0f || linear_speed_cm_s <= 0.0f) {
        return;
    }

    const float velocity = requested.linear * linear_speed_cm_s;
    const float velocity_x = velocity * cosf(pose->theta_rad);
    const float velocity_y = velocity * sinf(pose->theta_rad);
    float scale = min_scale_for_axis(pose->x_cm, velocity_x,
                                     bounds->min_x_cm, bounds->max_x_cm,
                                     bounds, dt_s);
    scale = fminf(scale, min_scale_for_axis(pose->y_cm, velocity_y,
                                            bounds->min_y_cm,
                                            bounds->max_y_cm,
                                            bounds, dt_s));
    limited->linear = requested.linear * clampf(scale, 0.0f, 1.0f);
    if (boundary_limited != NULL && scale < 0.999f) {
        *boundary_limited = true;
    }
}

void motion_math_integrate_pose(motion_pose_t *pose,
                                motion_twist_t twist,
                                float linear_speed_cm_s,
                                float angular_speed_deg_s,
                                float dt_s)
{
    if (pose == NULL || dt_s <= 0.0f) {
        return;
    }

    const float velocity = twist.linear * linear_speed_cm_s;
    pose->x_cm += velocity * cosf(pose->theta_rad) * dt_s;
    pose->y_cm += velocity * sinf(pose->theta_rad) * dt_s;
    pose->theta_rad += twist.angular * angular_speed_deg_s *
                       ((float)M_PI / 180.0f) * dt_s;

    while (pose->theta_rad > (float)M_PI) {
        pose->theta_rad -= 2.0f * (float)M_PI;
    }
    while (pose->theta_rad < -(float)M_PI) {
        pose->theta_rad += 2.0f * (float)M_PI;
    }
}
