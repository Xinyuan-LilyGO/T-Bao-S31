#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float motor_a;
    float motor_b;
} motion_wheel_mix_t;

typedef struct {
    float x_cm;
    float y_cm;
    float theta_rad;
} motion_pose_t;

typedef struct {
    float linear;
    float angular;
} motion_twist_t;

typedef struct {
    float min_x_cm;
    float max_x_cm;
    float min_y_cm;
    float max_y_cm;
    float soft_distance_cm;
    float hard_distance_cm;
} motion_bounds_t;

void motion_math_mix_wheels(float linear,
                            float angular,
                            float turn_gain,
                            float motor_a_sign,
                            float motor_b_sign,
                            motion_wheel_mix_t *out);

int motion_math_normalized_to_percent(float normalized,
                                      float minimum_percent,
                                      float maximum_percent);

float motion_math_ramp_toward(float current, float target, float max_delta);

void motion_math_limit_twist(const motion_pose_t *pose,
                             motion_twist_t requested,
                             float linear_speed_cm_s,
                             float dt_s,
                             const motion_bounds_t *bounds,
                             motion_twist_t *limited,
                             bool *boundary_limited);

void motion_math_integrate_pose(motion_pose_t *pose,
                                motion_twist_t twist,
                                float linear_speed_cm_s,
                                float angular_speed_deg_s,
                                float dt_s);

#ifdef __cplusplus
}
#endif
