#include "face_target_tracker.h"

#include <math.h>
#include <string.h>

#define FACE_TRACKER_DEFAULT_WIDTH 320
#define FACE_TRACKER_DEFAULT_HEIGHT 320
#define FACE_TRACKER_DEFAULT_SCORE 0.30f
/* The detector runs at about 7 Hz here, and a single skipped frame (motion
 * blur during a yaw pulse, a PARLIO receive timeout, a slow inference) puts
 * more than 300 ms between two accepted boxes. At the old 300 ms timeout two
 * missed frames - or one slow frame - already read as "face lost" while the
 * face was still in view. The lost decision now needs roughly six missed
 * updates, and the prediction window below covers the short gaps. */
#define FACE_TRACKER_DEFAULT_STALE_MS 800U
#define FACE_TRACKER_DEFAULT_REQUIRED_FRAMES 1U
#define FACE_TRACKER_DEFAULT_ALPHA 0.82f
/* Publish the extrapolated box across a couple of missed detections instead
 * of dropping to "no target" and parking the wheels. The old 70 ms window was
 * shorter than one detector interval, so it never fired. */
#define FACE_TRACKER_DEFAULT_PREDICTION_MS 400U
#define FACE_TRACKER_DEFAULT_PREDICTION_SPEED_PX_S 400.0f
#define FACE_TRACKER_DEFAULT_PREDICTION_DISTANCE_PX 40.0f
#define FACE_TRACKER_EDGE_REACQUIRE_MARGIN_PX 64
#define FACE_TRACKER_REQUIRED_VELOCITY_SAMPLES 2U
#define FACE_TRACKER_MIN_VELOCITY_PX_S 25.0f
#define FACE_TRACKER_MATCH_IOU_WEIGHT 0.55f
#define FACE_TRACKER_MATCH_DISTANCE_WEIGHT 0.40f
#define FACE_TRACKER_MATCH_SCORE_WEIGHT 0.05f
#define FACE_TRACKER_MIN_MATCH_SCORE 0.30f

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

static bool valid_config(const face_tracker_config_t *config)
{
    return config != NULL && config->frame_width > 0 &&
           config->frame_height > 0 && isfinite(config->minimum_score) &&
           config->minimum_score >= 0.0f && config->minimum_score <= 1.0f &&
           config->stale_timeout_ms > 0 &&
           config->required_consecutive_frames > 0 &&
           isfinite(config->smoothing_alpha) &&
           config->smoothing_alpha > 0.0f &&
           config->smoothing_alpha <= 1.0f &&
           config->prediction_max_ms > 0 &&
           isfinite(config->prediction_max_speed_px_s) &&
           config->prediction_max_speed_px_s > 0.0f &&
           isfinite(config->prediction_max_distance_px) &&
           config->prediction_max_distance_px > 0.0f;
}

static void clear_target(face_target_t *target)
{
    if (target != NULL) {
        memset(target, 0, sizeof(*target));
    }
}

static int box_area(const face_detection_t *detection)
{
    return (detection->x2 - detection->x1) *
           (detection->y2 - detection->y1);
}

static bool detection_is_valid(const face_tracker_t *tracker,
                               const face_detection_t *detection)
{
    if (tracker == NULL || detection == NULL ||
        !isfinite(detection->score) ||
        detection->score < tracker->config.minimum_score ||
        detection->score > 1.0f) {
        return false;
    }
    /* Detectors may return a box that extends a little beyond the image when
     * a face is entering from a side. Keep any box that intersects the frame;
     * clip_detection_to_frame() normalizes it before control. */
    return detection->x2 > 0 && detection->y2 > 0 &&
           detection->x1 < tracker->config.frame_width &&
           detection->y1 < tracker->config.frame_height &&
           detection->x2 > detection->x1 && detection->y2 > detection->y1;
}

static face_detection_t clip_detection_to_frame(
    const face_tracker_t *tracker, const face_detection_t *detection)
{
    face_detection_t clipped = *detection;
    if (clipped.x1 < 0) {
        clipped.x1 = 0;
    }
    if (clipped.y1 < 0) {
        clipped.y1 = 0;
    }
    if (clipped.x2 > tracker->config.frame_width) {
        clipped.x2 = tracker->config.frame_width;
    }
    if (clipped.y2 > tracker->config.frame_height) {
        clipped.y2 = tracker->config.frame_height;
    }
    return clipped;
}

static int select_initial_detection(const face_tracker_t *tracker,
                                    const face_detection_t *detections,
                                    size_t detection_count)
{
    if (tracker == NULL || detections == NULL) {
        return -1;
    }
    int selected = -1;
    for (size_t i = 0; i < detection_count; ++i) {
        if (!detection_is_valid(tracker, &detections[i])) {
            continue;
        }
        if (selected < 0 || detections[i].score > detections[selected].score ||
            (detections[i].score == detections[selected].score &&
             box_area(&detections[i]) > box_area(&detections[selected]))) {
            selected = (int)i;
        }
    }
    return selected;
}

static float intersection_over_union(const face_target_t *target,
                                     const face_detection_t *detection)
{
    const float left = fmaxf(target->x1, (float)detection->x1);
    const float top = fmaxf(target->y1, (float)detection->y1);
    const float right = fminf(target->x2, (float)detection->x2);
    const float bottom = fminf(target->y2, (float)detection->y2);
    const float intersection_width = fmaxf(0.0f, right - left);
    const float intersection_height = fmaxf(0.0f, bottom - top);
    const float intersection = intersection_width * intersection_height;
    const float target_area = fmaxf(0.0f, target->x2 - target->x1) *
                              fmaxf(0.0f, target->y2 - target->y1);
    const float detection_area = (float)box_area(detection);
    const float union_area = target_area + detection_area - intersection;
    return union_area > 0.0f ? intersection / union_area : 0.0f;
}

static float center_distance_score(const face_tracker_t *tracker,
                                   const face_target_t *target,
                                   const face_detection_t *detection)
{
    const float target_center_x = (target->x1 + target->x2) * 0.5f;
    const float target_center_y = (target->y1 + target->y2) * 0.5f;
    const float detection_center_x =
        ((float)detection->x1 + (float)detection->x2) * 0.5f;
    const float detection_center_y =
        ((float)detection->y1 + (float)detection->y2) * 0.5f;
    const float dx = detection_center_x - target_center_x;
    const float dy = detection_center_y - target_center_y;
    const float diagonal = sqrtf((float)(tracker->config.frame_width *
                                         tracker->config.frame_width) +
                                 (float)(tracker->config.frame_height *
                                         tracker->config.frame_height));
    if (diagonal <= 0.0f) {
        return 0.0f;
    }
    return 1.0f - clampf(sqrtf(dx * dx + dy * dy) / diagonal, 0.0f, 1.0f);
}

static int select_continuous_detection(const face_tracker_t *tracker,
                                       const face_detection_t *detections,
                                       size_t detection_count)
{
    if (tracker == NULL || detections == NULL) {
        return -1;
    }
    int selected = -1;
    float selected_score = FACE_TRACKER_MIN_MATCH_SCORE;
    for (size_t i = 0; i < detection_count; ++i) {
        if (!detection_is_valid(tracker, &detections[i])) {
            continue;
        }
        const float continuity =
            FACE_TRACKER_MATCH_IOU_WEIGHT *
                intersection_over_union(&tracker->target, &detections[i]) +
            FACE_TRACKER_MATCH_DISTANCE_WEIGHT *
                center_distance_score(tracker, &tracker->target,
                                      &detections[i]) +
            FACE_TRACKER_MATCH_SCORE_WEIGHT * detections[i].score;
        if (continuity > selected_score) {
            selected = (int)i;
            selected_score = continuity;
        }
    }
    return selected;
}

static int select_edge_reacquisition_detection(
    const face_tracker_t *tracker,
    const face_detection_t *detections,
    size_t detection_count)
{
    if (tracker == NULL || detections == NULL) {
        return -1;
    }

    const int margin = FACE_TRACKER_EDGE_REACQUIRE_MARGIN_PX;
    const int right_edge = tracker->config.frame_width - margin;
    int selected = -1;
    for (size_t i = 0; i < detection_count; ++i) {
        if (!detection_is_valid(tracker, &detections[i]) ||
            (detections[i].x1 > margin && detections[i].x2 < right_edge)) {
            continue;
        }
        if (selected < 0 ||
            detections[i].score > detections[selected].score ||
            (detections[i].score == detections[selected].score &&
             box_area(&detections[i]) > box_area(&detections[selected]))) {
            selected = (int)i;
        }
    }
    return selected;
}

static int select_single_valid_detection(const face_tracker_t *tracker,
                                         const face_detection_t *detections,
                                         size_t detection_count)
{
    if (tracker == NULL || detections == NULL || detection_count == 0) {
        return -1;
    }

    int selected = -1;
    for (size_t i = 0; i < detection_count; ++i) {
        if (!detection_is_valid(tracker, &detections[i])) {
            continue;
        }
        if (selected >= 0) {
            return -1;
        }
        selected = (int)i;
    }
    return selected;
}

static face_target_t target_from_detection(const face_tracker_t *tracker,
                                           const face_detection_t *detection)
{
    const face_detection_t clipped =
        clip_detection_to_frame(tracker, detection);
    face_target_t target = {
        .valid = true,
        .predicted = false,
        .score = clipped.score,
        .x1 = (float)clipped.x1,
        .y1 = (float)clipped.y1,
        .x2 = (float)clipped.x2,
        .y2 = (float)clipped.y2,
    };
    target.center_x = (target.x1 + target.x2) * 0.5f;
    target.center_y = (target.y1 + target.y2) * 0.5f;
    target.height_ratio = (target.y2 - target.y1) /
                          (float)tracker->config.frame_height;
    return target;
}

static void clamp_target(const face_tracker_t *tracker, face_target_t *target)
{
    const float frame_width = (float)tracker->config.frame_width;
    const float frame_height = (float)tracker->config.frame_height;
    float width = fmaxf(target->x2 - target->x1, 1.0f);
    float height = fmaxf(target->y2 - target->y1, 1.0f);
    width = fminf(width, frame_width);
    height = fminf(height, frame_height);
    target->x1 = clampf(target->x1, 0.0f, frame_width - width);
    target->y1 = clampf(target->y1, 0.0f, frame_height - height);
    target->x2 = target->x1 + width;
    target->y2 = target->y1 + height;
    target->center_x = (target->x1 + target->x2) * 0.5f;
    target->center_y = (target->y1 + target->y2) * 0.5f;
    target->height_ratio = clampf(
        (target->y2 - target->y1) / (float)tracker->config.frame_height,
        0.0f, 1.0f);
}

static void smooth_target(face_tracker_t *tracker,
                          const face_target_t *measurement)
{
    if (!tracker->target.valid) {
        tracker->target = *measurement;
        clamp_target(tracker, &tracker->target);
        return;
    }

    const float alpha = tracker->config.smoothing_alpha;
    tracker->target.x1 += alpha * (measurement->x1 - tracker->target.x1);
    tracker->target.y1 += alpha * (measurement->y1 - tracker->target.y1);
    tracker->target.x2 += alpha * (measurement->x2 - tracker->target.x2);
    tracker->target.y2 += alpha * (measurement->y2 - tracker->target.y2);
    tracker->target.score = measurement->score;
    tracker->target.valid = true;
    tracker->target.predicted = false;
    clamp_target(tracker, &tracker->target);
}

static void update_velocity(face_tracker_t *tracker,
                            const face_target_t *measurement,
                            uint32_t now_ms)
{
    if (tracker == NULL || measurement == NULL) {
        return;
    }

    if (!tracker->has_previous_measurement ||
        !tracker->has_valid_timestamp) {
        tracker->previous_measurement_center_x = measurement->center_x;
        tracker->previous_measurement_center_y = measurement->center_y;
        tracker->has_previous_measurement = true;
        tracker->velocity_x_px_s = 0.0f;
        tracker->velocity_y_px_s = 0.0f;
        tracker->velocity_streak = 0;
        tracker->has_velocity = false;
        return;
    }

    const uint32_t elapsed_ms = (uint32_t)(now_ms - tracker->last_valid_ms);
    const float delta_x =
        measurement->center_x - tracker->previous_measurement_center_x;
    const float delta_y =
        measurement->center_y - tracker->previous_measurement_center_y;
    tracker->previous_measurement_center_x = measurement->center_x;
    tracker->previous_measurement_center_y = measurement->center_y;
    if (elapsed_ms == 0U) {
        return;
    }

    const float dt_s = (float)elapsed_ms / 1000.0f;
    const float measured_x = delta_x / dt_s;
    const float measured_y = delta_y / dt_s;
    const float max_speed = tracker->config.prediction_max_speed_px_s;
    const float limited_x = clampf(measured_x, -max_speed, max_speed);
    const float limited_y = clampf(measured_y, -max_speed, max_speed);
    const float measured_speed = sqrtf(limited_x * limited_x +
                                       limited_y * limited_y);
    if (measured_speed < FACE_TRACKER_MIN_VELOCITY_PX_S) {
        tracker->velocity_x_px_s = 0.0f;
        tracker->velocity_y_px_s = 0.0f;
        tracker->velocity_streak = 0;
        tracker->has_velocity = false;
        return;
    }

    const bool same_direction =
        tracker->has_velocity &&
        (tracker->velocity_x_px_s * limited_x +
         tracker->velocity_y_px_s * limited_y) > 0.0f;
    if (same_direction) {
        const float velocity_alpha = 0.35f;
        tracker->velocity_x_px_s +=
            velocity_alpha * (limited_x - tracker->velocity_x_px_s);
        tracker->velocity_y_px_s +=
            velocity_alpha * (limited_y - tracker->velocity_y_px_s);
        if (tracker->velocity_streak < UINT8_MAX) {
            tracker->velocity_streak++;
        }
    } else {
        tracker->velocity_x_px_s = limited_x;
        tracker->velocity_y_px_s = limited_y;
        tracker->velocity_streak = 1;
        tracker->has_velocity = true;
    }
    tracker->velocity_x_px_s = clampf(tracker->velocity_x_px_s,
                                      -max_speed, max_speed);
    tracker->velocity_y_px_s = clampf(tracker->velocity_y_px_s,
                                      -max_speed, max_speed);
}

static bool make_predicted_target(const face_tracker_t *tracker,
                                  uint32_t now_ms,
                                  face_target_t *out_target)
{
    if (tracker == NULL || out_target == NULL || !tracker->locked ||
        !tracker->target.valid || !tracker->has_valid_timestamp) {
        return false;
    }

    const uint32_t elapsed_ms = (uint32_t)(now_ms - tracker->last_valid_ms);
    if (elapsed_ms == 0U || elapsed_ms > tracker->config.prediction_max_ms) {
        return false;
    }

    const float elapsed_s = (float)elapsed_ms / 1000.0f;
    float offset_x = 0.0f;
    float offset_y = 0.0f;
    if (tracker->has_velocity &&
        tracker->velocity_streak >= FACE_TRACKER_REQUIRED_VELOCITY_SAMPLES) {
        offset_x = tracker->velocity_x_px_s * elapsed_s;
        offset_y = tracker->velocity_y_px_s * elapsed_s;
    }
    const float max_distance = tracker->config.prediction_max_distance_px;
    const float distance = sqrtf(offset_x * offset_x + offset_y * offset_y);
    if (distance > max_distance && distance > 0.0f) {
        const float scale = max_distance / distance;
        offset_x *= scale;
        offset_y *= scale;
    }

    *out_target = tracker->target;
    out_target->predicted = true;
    out_target->x1 += offset_x;
    out_target->x2 += offset_x;
    out_target->y1 += offset_y;
    out_target->y2 += offset_y;
    clamp_target(tracker, out_target);
    return true;
}

static bool target_is_stale(const face_tracker_t *tracker, uint32_t now_ms)
{
    return tracker->locked && tracker->has_valid_timestamp &&
           (uint32_t)(now_ms - tracker->last_valid_ms) >=
               tracker->config.stale_timeout_ms;
}

void face_tracker_config_init(face_tracker_config_t *config)
{
    if (config == NULL) {
        return;
    }
    *config = (face_tracker_config_t){
        .frame_width = FACE_TRACKER_DEFAULT_WIDTH,
        .frame_height = FACE_TRACKER_DEFAULT_HEIGHT,
        .minimum_score = FACE_TRACKER_DEFAULT_SCORE,
        .stale_timeout_ms = FACE_TRACKER_DEFAULT_STALE_MS,
        .required_consecutive_frames = FACE_TRACKER_DEFAULT_REQUIRED_FRAMES,
        .smoothing_alpha = FACE_TRACKER_DEFAULT_ALPHA,
        .prediction_max_ms = FACE_TRACKER_DEFAULT_PREDICTION_MS,
        .prediction_max_speed_px_s = FACE_TRACKER_DEFAULT_PREDICTION_SPEED_PX_S,
        .prediction_max_distance_px =
            FACE_TRACKER_DEFAULT_PREDICTION_DISTANCE_PX,
    };
}

void face_tracker_init(face_tracker_t *tracker,
                       const face_tracker_config_t *config)
{
    if (tracker == NULL) {
        return;
    }
    face_tracker_config_t defaults;
    face_tracker_config_init(&defaults);
    tracker->config = valid_config(config) ? *config : defaults;
    face_tracker_reset(tracker);
}

void face_tracker_reset(face_tracker_t *tracker)
{
    if (tracker == NULL) {
        return;
    }
    clear_target(&tracker->target);
    tracker->state = FACE_TRACKER_ACQUIRING;
    tracker->last_valid_ms = 0;
    tracker->valid_streak = 0;
    tracker->locked = false;
    tracker->has_valid_timestamp = false;
    tracker->velocity_x_px_s = 0.0f;
    tracker->velocity_y_px_s = 0.0f;
    tracker->has_velocity = false;
    tracker->previous_measurement_center_x = 0.0f;
    tracker->previous_measurement_center_y = 0.0f;
    tracker->velocity_streak = 0;
    tracker->has_previous_measurement = false;
}

face_tracker_state_t face_tracker_update(face_tracker_t *tracker,
                                         const face_detection_t *detections,
                                         size_t detection_count,
                                         uint32_t now_ms,
                                         face_target_t *out_target)
{
    if (out_target != NULL) {
        clear_target(out_target);
    }
    if (tracker == NULL) {
        return FACE_TRACKER_LOST;
    }

    int selected = -1;
    if (tracker->locked) {
        selected = select_continuous_detection(tracker, detections,
                                               detection_count);
        if (selected < 0) {
            /* A fast lateral movement can make IoU briefly too small even
             * though the detector still sees the face at the image edge. */
            selected = select_edge_reacquisition_detection(
                tracker, detections, detection_count);
        }
        if (selected < 0 && detection_count == 1) {
            /* With exactly one valid face there is no identity ambiguity. Do
             * not leave the controller stuck in acquiring just because the
             * target moved quickly but has not reached the edge margin. */
            selected = select_single_valid_detection(
                tracker, detections, detection_count);
        }
    } else {
        selected = select_initial_detection(tracker, detections,
                                            detection_count);
    }

    if (selected < 0) {
        if (tracker->locked && target_is_stale(tracker, now_ms)) {
            face_tracker_reset(tracker);
            tracker->state = FACE_TRACKER_LOST;
            return tracker->state;
        }

        face_target_t predicted_target;
        if (make_predicted_target(tracker, now_ms, &predicted_target) &&
            tracker->valid_streak >=
                tracker->config.required_consecutive_frames) {
            tracker->state = FACE_TRACKER_FOLLOWING;
            if (out_target != NULL) {
                *out_target = predicted_target;
            }
            return tracker->state;
        }

        /* Keep the lock for reacquisition, but stop publishing a target as
         * soon as the short prediction window has expired. */
        tracker->valid_streak = 0;
        tracker->state = FACE_TRACKER_ACQUIRING;
        return tracker->state;
    }

    const face_target_t measurement =
        target_from_detection(tracker, &detections[selected]);
    update_velocity(tracker, &measurement, now_ms);
    smooth_target(tracker, &measurement);
    tracker->locked = true;
    tracker->has_valid_timestamp = true;
    tracker->last_valid_ms = now_ms;
    if (tracker->valid_streak < UINT8_MAX) {
        tracker->valid_streak++;
    }

    if (tracker->valid_streak >= tracker->config.required_consecutive_frames) {
        tracker->state = FACE_TRACKER_FOLLOWING;
        if (out_target != NULL) {
            *out_target = tracker->target;
            out_target->valid = true;
        }
    } else {
        tracker->state = FACE_TRACKER_ACQUIRING;
    }
    return tracker->state;
}
