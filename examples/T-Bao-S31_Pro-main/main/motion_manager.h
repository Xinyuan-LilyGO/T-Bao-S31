#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "motion_math.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MOTION_SOURCE_NONE = -1,
    MOTION_SOURCE_EXPRESSION = 0,
    MOTION_SOURCE_VOICE,
    MOTION_SOURCE_MCP,
    MOTION_SOURCE_TOUCH,
    MOTION_SOURCE_WEB,
    MOTION_SOURCE_SAFETY,
    MOTION_SOURCE_FOLLOW,
    MOTION_SOURCE_COUNT,
} motion_source_t;

#define MOTION_MAX_SEGMENTS 12

typedef struct {
    float linear;
    float angular;
    uint32_t ttl_ms;
    uint32_t ramp_ms;
    int servo_pulse_us;
    bool servo_enabled;
} motion_request_t;

typedef struct {
    float linear;
    float angular;
    uint32_t duration_ms;
    uint32_t ramp_ms;
    int servo_pulse_us;
    bool servo_enabled;
} motion_segment_t;

typedef struct {
    motion_pose_t pose;
    motion_twist_t requested_twist;
    motion_twist_t limited_twist;
    int current_motor_a_percent;
    int current_motor_b_percent;
    int current_servo_pulse_us;
    bool servo_enabled;
    int target_motor_a_percent;
    int target_motor_b_percent;
    bool boundary_limited;
    bool available;
    bool faulted;
    bool failsafe_active;
    bool sequence_active;
    motion_source_t active_source;
    size_t sequence_index;
    size_t sequence_count;
} motion_state_t;

esp_err_t motion_manager_init(void);
void motion_manager_stop(void);
esp_err_t motion_manager_submit(motion_source_t source,
                                const motion_request_t *request);
void motion_manager_clear_source(motion_source_t source);
esp_err_t motion_manager_start_sequence(motion_source_t source,
                                         const motion_segment_t *segments,
                                         size_t count);
void motion_manager_stop_all(void);
void motion_manager_reset_pose(void);
void motion_manager_get_state(motion_state_t *out_state);

#ifdef __cplusplus
}
#endif
