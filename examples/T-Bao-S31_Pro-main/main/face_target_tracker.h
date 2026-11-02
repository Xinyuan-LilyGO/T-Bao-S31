#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float score;
    int x1;
    int y1;
    int x2;
    int y2;
} face_detection_t;

typedef struct {
    bool valid;
    bool predicted;
    float score;
    float x1;
    float y1;
    float x2;
    float y2;
    float center_x;
    float center_y;
    float height_ratio;
} face_target_t;

typedef enum {
    FACE_TRACKER_ACQUIRING = 0,
    FACE_TRACKER_FOLLOWING,
    FACE_TRACKER_LOST,
} face_tracker_state_t;

typedef struct {
    int frame_width;
    int frame_height;
    float minimum_score;
    uint32_t stale_timeout_ms;
    uint8_t required_consecutive_frames;
    float smoothing_alpha;
    uint32_t prediction_max_ms;
    float prediction_max_speed_px_s;
    float prediction_max_distance_px;
} face_tracker_config_t;

typedef struct {
    face_tracker_config_t config;
    face_target_t target;
    face_tracker_state_t state;
    uint32_t last_valid_ms;
    uint8_t valid_streak;
    bool locked;
    bool has_valid_timestamp;
    float velocity_x_px_s;
    float velocity_y_px_s;
    bool has_velocity;
    float previous_measurement_center_x;
    float previous_measurement_center_y;
    uint8_t velocity_streak;
    bool has_previous_measurement;
} face_tracker_t;

void face_tracker_config_init(face_tracker_config_t *config);

void face_tracker_init(face_tracker_t *tracker,
                       const face_tracker_config_t *config);

void face_tracker_reset(face_tracker_t *tracker);

face_tracker_state_t face_tracker_update(face_tracker_t *tracker,
                                         const face_detection_t *detections,
                                         size_t detection_count,
                                         uint32_t now_ms,
                                         face_target_t *out_target);

#ifdef __cplusplus
}
#endif
