#include "app_face_follow.h"

#include <list>
#include <math.h>
#include <stddef.h>
#include <string.h>

#include "sdkconfig.h"

#include "app_activity.h"
#include "app_camera.h"
#include "app_camera_preview.h"
#include "esp_heap_caps.h"
#include "esp_heap_caps_init.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "human_face_detect.hpp"
#include "motion_manager.h"

static const char *TAG = "FACE_FOLLOW";

#define FACE_FOLLOW_REQUEST_TTL_MS 120U
/* The yaw loop drives the wheels in 50-200 ms pulses, so the motor ramp has
 * to reach the commanded duty inside that window. At the old 180 ms the wheel
 * was still below the gearbox breakaway point when the pulse ended. */
#define FACE_FOLLOW_REQUEST_RAMP_MS 60U
#define FACE_FOLLOW_HEARTBEAT_MS 40U
#define FACE_FOLLOW_COMMAND_MAX_AGE_MS 180U
#define FACE_FOLLOW_TASK_DELAY_MS 35U
#define FACE_FOLLOW_FAILURE_DELAY_MS 200U
#define FACE_FOLLOW_ERROR_LOG_INTERVAL_MS 2000U
#define FACE_FOLLOW_MODEL_LOG_INTERVAL_MS 300U
#define FACE_FOLLOW_MAX_DETECTIONS 10U
#define FACE_FOLLOW_START_SERVO_PULSE_US 1800
/* Detector stage score thresholds (model default is 0.5 for both). */
#define FACE_FOLLOW_DETECTOR_MSR_SCORE 0.40f
#define FACE_FOLLOW_DETECTOR_MNP_SCORE 0.40f

static SemaphoreHandle_t s_mutex;
static SemaphoreHandle_t s_inference_exit_sem;
static SemaphoreHandle_t s_heartbeat_exit_sem;
static TaskHandle_t s_task;
static TaskHandle_t s_heartbeat_task;
static bool s_initialized;
static bool s_enabled;
static bool s_background_enabled;
static face_follow_status_t s_status;
/* Captured before the worker tasks start. The inference task must not take a
 * second snapshot while the heartbeat is still handing the seed to the motion
 * manager, or a stale 1500 us state can overwrite the real head position. */
static int s_start_servo_pulse_us = FACE_FOLLOW_SERVO_PULSE_CENTER_US;
static face_follow_state_t s_last_logged_state = FACE_FOLLOW_STATE_DISABLED;
static uint32_t s_last_detection_log_ms;
static bool s_detection_log_initialized;
/* Detector period, logged so the yaw brake lead can be tuned against the real
 * inference rate instead of a guess. */
static uint32_t s_previous_sample_ms;
static uint32_t s_sample_period_ms;
/* Loaded once and kept for the lifetime of the application.  Re-loading the
 * MSRMNP models on every face-follow start performs a large amount of small
 * internal-RAM allocation, which fragmenting the internal heap (and made a
 * concurrent XiaoZhi wake fail to allocate its PHY timer).  Reusing the model
 * also makes subsequent face-follow starts immediate. */
static HumanFaceDetect *s_detector = nullptr;

static const char *follow_state_name(face_follow_state_t state)
{
    switch (state) {
    case FACE_FOLLOW_STATE_STARTING:
        return "starting";
    case FACE_FOLLOW_STATE_ACQUIRING:
        return "acquiring";
    case FACE_FOLLOW_STATE_FOLLOWING:
        return "following";
    case FACE_FOLLOW_STATE_LOST:
        return "lost";
    case FACE_FOLLOW_STATE_ERROR:
        return "error";
    case FACE_FOLLOW_STATE_DISABLED:
    default:
        return "disabled";
    }
}

static const char *stop_reason_name(face_follow_stop_reason_t reason)
{
    switch (reason) {
    case FACE_FOLLOW_STOP_TARGET_SIZE:
        return "target_size";
    case FACE_FOLLOW_STOP_TOO_CLOSE:
        return "too_close";
    case FACE_FOLLOW_STOP_PREDICTED_TARGET:
        return "predicted_target";
    case FACE_FOLLOW_STOP_STALE_TARGET:
        return "stale_target";
    case FACE_FOLLOW_STOP_INVALID_TARGET:
        return "invalid_target";
    case FACE_FOLLOW_STOP_DIRECTION_BRAKE:
        return "direction_brake";
    case FACE_FOLLOW_STOP_FAR_TARGET:
        return "far_target";
    case FACE_FOLLOW_STOP_NONE:
    default:
        return "none";
    }
}

static const char *tracker_state_name(face_tracker_state_t state)
{
    switch (state) {
    case FACE_TRACKER_FOLLOWING:
        return "following";
    case FACE_TRACKER_LOST:
        return "lost";
    case FACE_TRACKER_ACQUIRING:
    default:
        return "acquiring";
    }
}

static face_follow_command_t zero_command(face_follow_stop_reason_t reason)
{
    return face_follow_command_t{
        0.0f,
        0.0f,
        reason,
        FACE_FOLLOW_SERVO_PULSE_CENTER_US,
        false,
        0,
    };
}

static int clamp_servo_pulse_us(int pulse_us)
{
    if (pulse_us < FACE_FOLLOW_SERVO_PULSE_MIN_US) {
        return FACE_FOLLOW_SERVO_PULSE_MIN_US;
    }
    if (pulse_us > FACE_FOLLOW_SERVO_PULSE_MAX_US) {
        return FACE_FOLLOW_SERVO_PULSE_MAX_US;
    }
    return pulse_us;
}

static int get_start_servo_pulse_us(void)
{
    if (s_mutex != NULL && xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
        const int pulse_us = s_start_servo_pulse_us;
        xSemaphoreGive(s_mutex);
        return clamp_servo_pulse_us(pulse_us);
    }
    return FACE_FOLLOW_SERVO_PULSE_CENTER_US;
}

static face_follow_command_t hold_servo_command(
    int servo_pulse_us, face_follow_stop_reason_t reason)
{
    return face_follow_command_t{
        0.0f,
        0.0f,
        reason,
        clamp_servo_pulse_us(servo_pulse_us),
        true,
        0,
    };
}

static face_follow_command_t hold_command(
    const face_follow_command_t *last_command,
    face_follow_stop_reason_t reason)
{
    if (last_command != NULL && last_command->servo_enabled) {
        return hold_servo_command(last_command->servo_pulse_us, reason);
    }
    return zero_command(reason);
}

static bool is_enabled_locked(void)
{
    return s_enabled;
}

static bool is_enabled(void)
{
    if (s_mutex == NULL || xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    const bool enabled = is_enabled_locked();
    xSemaphoreGive(s_mutex);
    return enabled;
}

static void set_zero_status(face_follow_stop_reason_t reason,
                            face_follow_state_t state,
                            uint32_t now_ms,
                            esp_err_t error)
{
    if (s_mutex == NULL || xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    if (s_enabled) {
        s_status.state = state;
        s_status.target = face_target_t{};
        s_status.command = hold_command(&s_status.command, reason);
        s_status.last_update_ms = now_ms;
        if (error != ESP_OK) {
            s_status.last_error = error;
        }
    }
    xSemaphoreGive(s_mutex);
}

static void set_detection_status(face_tracker_state_t tracker_state,
                                 const face_target_t *target,
                                 const face_follow_command_t *command,
                                 uint32_t now_ms)
{
    face_follow_status_t snapshot = {};
    bool should_log = false;
    if (s_mutex == NULL || xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    if (s_enabled) {
        s_status.state = tracker_state == FACE_TRACKER_FOLLOWING
                             ? FACE_FOLLOW_STATE_FOLLOWING
                             : tracker_state == FACE_TRACKER_LOST
                                   ? FACE_FOLLOW_STATE_LOST
                                   : FACE_FOLLOW_STATE_ACQUIRING;
        s_status.target = target != NULL ? *target : face_target_t{};
        s_status.command = command != NULL
                               ? *command
                               : zero_command(FACE_FOLLOW_STOP_INVALID_TARGET);
        s_status.last_update_ms = now_ms;
        s_status.last_error = ESP_OK;
        snapshot = s_status;
        should_log = !s_detection_log_initialized ||
                     snapshot.state != s_last_logged_state ||
                     (uint32_t)(now_ms - s_last_detection_log_ms) >= 1000U;
    }
    xSemaphoreGive(s_mutex);

    if (!should_log) {
        return;
    }
    s_detection_log_initialized = true;
    s_last_logged_state = snapshot.state;
    s_last_detection_log_ms = now_ms;
    if (snapshot.target.valid) {
        ESP_LOGI(TAG,
                 "state=%s box=(%.0f,%.0f)-(%.0f,%.0f) "
                 "center=(%.0f,%.0f) face_h_ratio=%.3f "
                 "score=%.2f source=%s "
                 "servo_us=%d servo=%s "
                 "cmd=(linear=%.2f angular=%.2f stop=%s)",
                  follow_state_name(snapshot.state),
                  snapshot.target.x1,
                  snapshot.target.y1, snapshot.target.x2, snapshot.target.y2,
                  snapshot.target.center_x, snapshot.target.center_y,
                  snapshot.target.height_ratio,
                  snapshot.target.score,
                  snapshot.target.predicted ? "predicted" : "measured",
                  snapshot.command.servo_pulse_us,
                  snapshot.command.servo_enabled ? "on" : "off",
                  snapshot.command.linear,
                  snapshot.command.angular,
                  stop_reason_name(snapshot.command.stop_reason));
    } else {
        ESP_LOGI(TAG,
                 "state=%s face=none servo_us=%d servo=%s "
                 "cmd=(linear=%.2f angular=%.2f stop=%s)",
                 follow_state_name(snapshot.state),
                 snapshot.command.servo_pulse_us,
                 snapshot.command.servo_enabled ? "on" : "off",
                 snapshot.command.linear,
                 snapshot.command.angular,
                 stop_reason_name(snapshot.command.stop_reason));
    }
}

static void log_model_diagnostics(
    const face_detection_t *detections,
    size_t detection_count,
    const face_target_t *target,
    face_tracker_state_t tracker_state,
    const face_follow_command_t *command,
    const face_follow_controller_config_t *config,
    uint32_t now_ms,
    uint32_t *last_log_ms)
{
    if (s_previous_sample_ms != 0U) {
        s_sample_period_ms = (uint32_t)(now_ms - s_previous_sample_ms);
    }
    s_previous_sample_ms = now_ms;

    if (last_log_ms != NULL && *last_log_ms != 0 &&
        (uint32_t)(now_ms - *last_log_ms) <
            FACE_FOLLOW_MODEL_LOG_INTERVAL_MS) {
        return;
    }
    if (last_log_ms != NULL) {
        *last_log_ms = now_ms;
    }

    const face_follow_command_t empty_command =
        zero_command(FACE_FOLLOW_STOP_INVALID_TARGET);
    const face_follow_command_t &reported_command =
        command != NULL ? *command : empty_command;

    ESP_LOGI(TAG, "model_diag count=%u tracker=%s",
             (unsigned)detection_count, tracker_state_name(tracker_state));
    for (size_t i = 0; i < detection_count; ++i) {
        ESP_LOGI(TAG,
                 "model[%u/%u] score=%.3f box=(%d,%d)-(%d,%d)",
                 (unsigned)(i + 1), (unsigned)detection_count,
                 detections[i].score, detections[i].x1, detections[i].y1,
                 detections[i].x2, detections[i].y2);
    }

    if (target == NULL || !target->valid || config == NULL ||
        config->frame_width <= 0 || config->frame_height <= 0) {
        ESP_LOGI(TAG,
                 "follow_diag target=none servo_us=%d servo=%d "
                 "cmd=(linear=%.3f angular=%.3f stop=%s)",
                 reported_command.servo_pulse_us,
                 reported_command.servo_enabled ? 1 : 0,
                 reported_command.linear, reported_command.angular,
                 stop_reason_name(reported_command.stop_reason));
        return;
    }

    const float half_width = (float)config->frame_width * 0.5f;
    const float half_height = (float)config->frame_height * 0.5f;
    const float x_error = half_width > 0.0f
                              ? (target->center_x - half_width) / half_width
                              : 0.0f;
    const float y_error = half_height > 0.0f
                              ? (target->center_y - half_height) / half_height
                              : 0.0f;
    const float yaw_band_px = config->yaw_deadband_ratio * half_width;
    const bool x_centered =
        fabsf(target->center_x - half_width) <= yaw_band_px;
    const bool y_centered = target->y1 >= config->target_box_y1 &&
                            target->y2 <= config->target_box_y2;
    const bool near_by_box =
        target->x1 <= config->near_box_x1 &&
        target->y1 <= config->near_box_y1 &&
        target->x2 >= config->near_box_x2 &&
        target->y2 >= config->near_box_y2;
    const bool near_by_height =
        target->height_ratio >= config->near_height_ratio;
    const bool far_by_height = target->height_ratio < config->far_height_ratio;

    ESP_LOGI(TAG,
             "follow_diag target_box=(%.1f,%.1f)-(%.1f,%.1f) "
             "yaw_band=+-%.1fpx (release=+-%.1fpx) "
             "center=(%.1f,%.1f) center_offset=(x=%.3f y=%.3f) "
             "inside=(x=%d y=%d) face_h=%.3f "
             "far_by_height=%d near_by_height=%d near_by_box=%d "
             "source=%s sample_ms=%u servo_us=%d servo=%d "
             "cmd=(linear=%.3f angular=%.3f pulse=%ums stop=%s)",
             target->x1, target->y1, target->x2, target->y2,
             yaw_band_px, config->yaw_release_ratio * half_width,
             target->center_x, target->center_y, x_error, y_error,
             x_centered ? 1 : 0, y_centered ? 1 : 0, target->height_ratio,
             far_by_height ? 1 : 0, near_by_height ? 1 : 0,
             near_by_box ? 1 : 0, target->predicted ? "predicted" : "measured",
             (unsigned)s_sample_period_ms,
             reported_command.servo_pulse_us,
             reported_command.servo_enabled ? 1 : 0,
             reported_command.linear, reported_command.angular,
             (unsigned)reported_command.turn_pulse_ms,
             stop_reason_name(reported_command.stop_reason));
}

static esp_err_t submit_follow_command(const face_follow_command_t *command)
{
    if (command == NULL || s_mutex == NULL ||
        xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_enabled) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    const motion_request_t request = {
        command->linear,
        command->angular,
        FACE_FOLLOW_REQUEST_TTL_MS,
        FACE_FOLLOW_REQUEST_RAMP_MS,
        command->servo_pulse_us,
        command->servo_enabled,
    };
    const esp_err_t ret = motion_manager_submit(MOTION_SOURCE_FOLLOW,
                                                &request);
    if (ret != ESP_OK) {
        s_status.last_error = ret;
    }
    xSemaphoreGive(s_mutex);
    return ret;
}

static void rate_limited_error(const char *stage, esp_err_t error,
                               uint32_t now_ms, uint32_t *last_log_ms)
{
    if (last_log_ms == NULL || *last_log_ms == 0 ||
        (uint32_t)(now_ms - *last_log_ms) >=
            FACE_FOLLOW_ERROR_LOG_INTERVAL_MS) {
        if (last_log_ms != NULL) {
            *last_log_ms = now_ms;
        }
        ESP_LOGW(TAG, "%s failed: %s; follow remains stopped", stage,
                 esp_err_to_name(error));
    }
}

static size_t convert_detections(
    const std::list<dl::detect::result_t> &results,
    face_detection_t *detections,
    size_t capacity)
{
    if (detections == NULL || capacity == 0) {
        return 0;
    }

    size_t count = 0;
    for (const dl::detect::result_t &result : results) {
        if (count >= capacity) {
            break;
        }
        if (result.box.size() < 4) {
            continue;
        }
        detections[count++] = face_detection_t{
            result.score,
            result.box[0],
            result.box[1],
            result.box[2],
            result.box[3],
        };
    }
    return count;
}

static void finish_inference_task(bool fatal, esp_err_t error)
{
    if (s_mutex != NULL && xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
        if (fatal) {
            s_enabled = false;
            s_background_enabled = false;
            s_status.enabled = false;
            s_status.state = FACE_FOLLOW_STATE_ERROR;
            s_status.last_error = error;
        } else if (!s_enabled) {
            s_status.enabled = false;
            s_status.state = FACE_FOLLOW_STATE_DISABLED;
        }
        xSemaphoreGive(s_mutex);
    }
    if (fatal) {
        ESP_LOGE(TAG, "inference task stopped: %s", esp_err_to_name(error));
    }
    motion_manager_clear_source(MOTION_SOURCE_FOLLOW);
    app_activity_camera_release(APP_ACT_CAMERA_FOLLOW);
    if (s_mutex != NULL && xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
        s_task = NULL;
        xSemaphoreGive(s_mutex);
    }
    if (s_inference_exit_sem != NULL) {
        xSemaphoreGive(s_inference_exit_sem);
    }
}

static void follow_heartbeat_task(void *arg)
{
    (void)arg;
    /* Start of the pulse for the command currently being repeated. Measured
     * from the first tick that carried the command, not from the inference
     * timestamp, because up to one heartbeat period can pass in between. */
    uint32_t pulse_start_ms = 0;
    uint32_t pulse_source_ms = 0;
    while (true) {
        face_follow_command_t command =
            zero_command(FACE_FOLLOW_STOP_STALE_TARGET);
        bool active = false;
        const uint32_t now_ms =
            (uint32_t)(esp_timer_get_time() / 1000ULL);

        if (s_mutex != NULL &&
            xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
            active = s_enabled;
            if (active &&
                (uint32_t)(now_ms - s_status.last_update_ms) <=
                    FACE_FOLLOW_COMMAND_MAX_AGE_MS) {
                command = s_status.command;
                /* A yaw correction is a pulse: the chassis turns much faster
                 * than the detector runs, so the turn is dropped once its
                 * window has elapsed and the brake ramp stops the wheels.
                 * The camera pitch pulse is unaffected. */
                if (command.turn_pulse_ms > 0U) {
                    if (s_status.last_update_ms != pulse_source_ms) {
                        pulse_source_ms = s_status.last_update_ms;
                        pulse_start_ms = now_ms;
                    }
                    if ((uint32_t)(now_ms - pulse_start_ms) >=
                        command.turn_pulse_ms) {
                        command.angular = 0.0f;
                    }
                }
            } else if (active) {
                /* Inference can take longer than the command TTL. Stop the
                 * wheels, but keep the last camera angle until inference
                 * explicitly reports a real LOST state. */
                command = hold_command(&s_status.command,
                                       FACE_FOLLOW_STOP_STALE_TARGET);
            }
            xSemaphoreGive(s_mutex);
        }
        if (!active) {
            break;
        }
        (void)submit_follow_command(&command);
        vTaskDelay(pdMS_TO_TICKS(FACE_FOLLOW_HEARTBEAT_MS));
    }

    if (s_mutex != NULL && xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
        s_heartbeat_task = NULL;
        xSemaphoreGive(s_mutex);
    }
    if (s_heartbeat_exit_sem != NULL) {
        xSemaphoreGive(s_heartbeat_exit_sem);
    }
    vTaskDelete(NULL);
}

/* esp-dl overrides dl::module::Module::operator new() to call
 * heap_caps_aligned_alloc(16, size, MALLOC_CAP_DEFAULT).  A MALLOC_CAP_DEFAULT
 * request can be served by internal RAM and the allocator tries the internal
 * regions first, so building the two MSRMNP graphs drains internal SRAM even
 * though 13 MiB of PSRAM sit unused.  While that happens the WiFi power-save
 * path can call esp_timer_create() for the PLL-tracking timer and
 * ESP_ERROR_CHECK aborts the device on the transient ESP_ERR_NO_MEM
 * (phy_track_pll_init), which is the "enable XiaoZhi, enter face-follow"
 * reboot.
 *
 * Register a small PSRAM-backed heap that advertises only MALLOC_CAP_DEFAULT
 * (plus the data-access bits, same priority layout as the regular PSRAM
 * region, so SIMD-capable DEFAULT requests match).  Newly added heaps are
 * linked at the head of the allocator's heap list, so MALLOC_CAP_DEFAULT-only
 * requests (the DL module objects and tables) are now served from PSRAM.
 * Every request that also asks for INTERNAL or SPIRAM - FreeRTOS objects,
 * task stacks, esp_timer, DMA buffers and the other PSRAM users - cannot
 * match this region and keeps using its previous heap, so the region stays
 * dedicated to the loader and cannot be filled by video/camera buffers. */
static void face_detector_psram_region_init(void)
{
    static bool s_region_ready = false;
    if (s_region_ready) {
        return;
    }
    const size_t region_bytes = 256 * 1024;
    uint8_t *region = static_cast<uint8_t *>(heap_caps_aligned_alloc(
        64, region_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (region == NULL) {
        ESP_LOGW(TAG, "Detector metadata region: PSRAM allocation failed");
        return;
    }
    const uint32_t region_caps[] = {
        MALLOC_CAP_DEFAULT,
        0,
        MALLOC_CAP_8BIT | MALLOC_CAP_32BIT | MALLOC_CAP_SIMD,
    };
    const esp_err_t err = heap_caps_add_region_with_caps(
        region_caps, (intptr_t)region, (intptr_t)(region + region_bytes - 1));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Detector metadata region registration failed: %s",
                 esp_err_to_name(err));
        heap_caps_free(region);
        return;
    }
    s_region_ready = true;
    ESP_LOGI(TAG, "Detector metadata heap: %u KiB PSRAM region at %p",
             (unsigned)(region_bytes / 1024), (void *)region);
}

static void face_follow_task(void *arg)
{
    (void)arg;
    uint8_t *rgb_buffer = static_cast<uint8_t *>(heap_caps_aligned_alloc(
        64, CAM_PREVIEW_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (rgb_buffer == NULL) {
        ESP_LOGE(TAG, "RGB565 buffer allocation failed");
        finish_inference_task(true, ESP_ERR_NO_MEM);
        vTaskDelete(NULL);
        return;
    }

    if (s_detector == nullptr) {
        ESP_LOGI(TAG, "loading face detector: internal free=%u largest=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        /* Route the loader's MALLOC_CAP_DEFAULT allocations (esp-dl module
         * objects allocated by its overridden operator new) into PSRAM. */
        face_detector_psram_region_init();
        /* Constructing the two MSRMNP graphs also allocates plain new/malloc
         * objects (maps, strings, vectors).  With the default 4 KiB
         * always-internal policy those exhaust the last few KiB of internal
         * SRAM; while that window is open, the WiFi power-save path calls
         * esp_timer_create() for the PLL-tracking timer and ESP_ERROR_CHECK
         * aborts the whole device on the transient ESP_ERR_NO_MEM
         * (phy_track_pll_init).  Prefer PSRAM while the model is built: the
         * detector's tensors already live there and 13 MiB are free, so this
         * only moves cold metadata.  The policy is restored immediately after
         * the constructor returns, keeping every other task unaffected
         * outside this short window. */
        heap_caps_malloc_extmem_enable(0);
        HumanFaceDetect *detector =
            new HumanFaceDetect(HumanFaceDetect::MSRMNP_S8_V1, false);
        heap_caps_malloc_extmem_enable(CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL);
        if (detector == NULL || detector->get_raw_model(0) == NULL ||
            detector->get_raw_model(1) == NULL) {
            ESP_LOGE(TAG, "MSRMNP model is not available");
            delete detector;
            heap_caps_free(rgb_buffer);
            finish_inference_task(true, ESP_ERR_NOT_FOUND);
            vTaskDelete(NULL);
            return;
        }
        /* Both MSRMNP stages default to a 0.5 score threshold, which drops the
         * box as soon as the face turns, blurs during a yaw pulse or is partly
         * out of frame - one or two of those frames in a row used to read as
         * "face lost". A lower threshold keeps the box alive; the tracker's
         * own minimum_score still rejects junk, and continuity matching keeps
         * the lock on the face that is already being followed. */
        detector->set_score_thr(FACE_FOLLOW_DETECTOR_MSR_SCORE, 0);
        detector->set_score_thr(FACE_FOLLOW_DETECTOR_MNP_SCORE, 1);
        s_detector = detector;
        ESP_LOGI(TAG, "face detector loaded once; later sessions reuse it "
                 "(score>=%.2f/%.2f internal free=%u largest=%u)",
                 FACE_FOLLOW_DETECTOR_MSR_SCORE, FACE_FOLLOW_DETECTOR_MNP_SCORE,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }

    face_tracker_config_t tracker_config;
    face_tracker_config_init(&tracker_config);
    ESP_LOGI(TAG,
             "tracker config: score>=%.2f stale=%ums acquire=%u alpha=%.2f "
             "predict=%ums speed<=%.0fpx/s distance<=%.0fpx",
             tracker_config.minimum_score,
             (unsigned)tracker_config.stale_timeout_ms,
             (unsigned)tracker_config.required_consecutive_frames,
             tracker_config.smoothing_alpha,
             (unsigned)tracker_config.prediction_max_ms,
             tracker_config.prediction_max_speed_px_s,
             tracker_config.prediction_max_distance_px);
    face_tracker_t tracker;
    face_tracker_init(&tracker, &tracker_config);

    face_follow_controller_config_t controller_config;
    face_follow_controller_config_init(&controller_config);
    ESP_LOGI(TAG,
             "yaw config: drive=%.2f (predicted %.2f) "
             "pulse=%u..%ums %.2fms/px band=%.2f release=%.2f "
             "right-scale=%.2f "
             "pitch kp=%.1f ki=%.1f kd=%.1f correction<=%.1fus",
             controller_config.yaw_drive,
             controller_config.yaw_predicted_drive,
             (unsigned)controller_config.yaw_pulse_ms,
             (unsigned)controller_config.yaw_pulse_max_ms,
             controller_config.yaw_pulse_ms_per_px,
             controller_config.yaw_deadband_ratio,
             controller_config.yaw_release_ratio,
             controller_config.yaw_right_pulse_scale,
             controller_config.vertical_kp, controller_config.vertical_ki,
             controller_config.vertical_kd,
             controller_config.max_servo_correction_us);
    ESP_LOGI(TAG,
             "follow zones: pitch_box_y=(%.0f,%.0f) "
             "near_box=(%.0f,%.0f)-(%.0f,%.0f) "
             "release_box=(%.0f,%.0f)-(%.0f,%.0f) "
             "far_height<%.3f near_height>=%.3f release_height>=%.3f "
             "confirm=%u release=%u",
             controller_config.target_box_y1,
             controller_config.target_box_y2,
             controller_config.near_box_x1, controller_config.near_box_y1,
             controller_config.near_box_x2, controller_config.near_box_y2,
             controller_config.near_release_box_x1,
             controller_config.near_release_box_y1,
             controller_config.near_release_box_x2,
             controller_config.near_release_box_y2,
             controller_config.far_height_ratio,
             controller_config.near_height_ratio,
             controller_config.near_release_height_ratio,
             (unsigned)controller_config.near_confirm_frames,
             (unsigned)controller_config.near_release_frames);
    face_follow_controller_t controller;
    face_follow_controller_init(&controller, &controller_config);
    const int initial_servo_pulse_us = get_start_servo_pulse_us();
    face_follow_controller_set_servo_pulse(&controller, initial_servo_pulse_us);
    ESP_LOGI(TAG, "servo follow starts from pulse_us=%d (no center reset)",
             initial_servo_pulse_us);

    face_follow_command_t last_command = hold_servo_command(
        initial_servo_pulse_us, FACE_FOLLOW_STOP_STALE_TARGET);

    uint32_t last_error_log_ms = 0;
    uint32_t last_model_log_ms = 0;
    while (is_enabled()) {
        uint32_t now_ms =
            (uint32_t)(esp_timer_get_time() / 1000ULL);
        if (!app_camera_is_running()) {
            set_zero_status(FACE_FOLLOW_STOP_STALE_TARGET,
                            FACE_FOLLOW_STATE_ACQUIRING,
                            now_ms, ESP_ERR_INVALID_STATE);
            vTaskDelay(pdMS_TO_TICKS(FACE_FOLLOW_FAILURE_DELAY_MS));
            continue;
        }

        uint8_t *jpeg = NULL;
        size_t jpeg_len = 0;
        esp_err_t ret = app_camera_capture_jpeg(&jpeg, &jpeg_len);
        if (ret != ESP_OK) {
            app_camera_release_jpeg(jpeg);
            set_zero_status(FACE_FOLLOW_STOP_STALE_TARGET,
                            FACE_FOLLOW_STATE_ACQUIRING,
                            now_ms, ret);
            rate_limited_error("camera capture", ret, now_ms,
                               &last_error_log_ms);
            vTaskDelay(pdMS_TO_TICKS(FACE_FOLLOW_FAILURE_DELAY_MS));
            continue;
        }

        ret = app_camera_preview_process(jpeg, jpeg_len, rgb_buffer);
        app_camera_release_jpeg(jpeg);
        if (ret != ESP_OK) {
            set_zero_status(FACE_FOLLOW_STOP_INVALID_TARGET,
                            FACE_FOLLOW_STATE_ACQUIRING,
                            now_ms, ret);
            rate_limited_error("JPEG decode", ret, now_ms,
                               &last_error_log_ms);
            vTaskDelay(pdMS_TO_TICKS(FACE_FOLLOW_FAILURE_DELAY_MS));
            continue;
        }

        dl::image::img_t image = {
            rgb_buffer,
            CAM_PREVIEW_W,
            CAM_PREVIEW_H,
            dl::image::DL_IMAGE_PIX_TYPE_RGB565LE,
        };
        std::list<dl::detect::result_t> &results = s_detector->run(image);
        now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
        face_detection_t detections[FACE_FOLLOW_MAX_DETECTIONS];
        const size_t detection_count = convert_detections(
            results, detections, FACE_FOLLOW_MAX_DETECTIONS);
        face_target_t target = {0};
        const face_tracker_state_t tracker_state = face_tracker_update(
            &tracker, detections, detection_count, now_ms, &target);

        face_follow_command_t command = zero_command(
            tracker_state == FACE_TRACKER_LOST
                ? FACE_FOLLOW_STOP_STALE_TARGET
                : FACE_FOLLOW_STOP_INVALID_TARGET);
        if (tracker_state == FACE_TRACKER_FOLLOWING && target.valid) {
            command = face_follow_controller_step(&controller, &target, true,
                                                  now_ms);
            last_command = command;
        } else if (tracker_state == FACE_TRACKER_LOST) {
            /* The tracker has exceeded stale_timeout_ms. This is the one
             * point where a new target must start with clean PID state. Keep
             * the camera at its last position while waiting for reacquisition. */
            face_follow_controller_reset(&controller);
            last_command = hold_command(&last_command,
                                        FACE_FOLLOW_STOP_STALE_TARGET);
            command = last_command;
        } else {
            /* ACQUIRING is often only one missed detector frame. Park the
             * wheels, but keep the last measured servo position and PID
             * state for reacquisition. */
            command = hold_command(&last_command,
                                   FACE_FOLLOW_STOP_STALE_TARGET);
        }
        log_model_diagnostics(detections, detection_count, &target,
                              tracker_state, &command, &controller_config,
                              now_ms, &last_model_log_ms);
        set_detection_status(tracker_state, &target, &command, now_ms);
        vTaskDelay(pdMS_TO_TICKS(FACE_FOLLOW_TASK_DELAY_MS));
    }

    heap_caps_free(rgb_buffer);
    /* s_detector is intentionally kept alive for the next face-follow session. */
    finish_inference_task(false, ESP_OK);
    vTaskDelete(NULL);
}

esp_err_t app_face_follow_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
        if (s_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_inference_exit_sem == NULL) {
        s_inference_exit_sem = xSemaphoreCreateBinary();
        if (s_inference_exit_sem == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_heartbeat_exit_sem == NULL) {
        s_heartbeat_exit_sem = xSemaphoreCreateBinary();
        if (s_heartbeat_exit_sem == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    s_status = face_follow_status_t{};
    s_status.state = FACE_FOLLOW_STATE_DISABLED;
    s_status.last_error = ESP_OK;
    s_enabled = false;
    s_background_enabled = false;
    s_start_servo_pulse_us = FACE_FOLLOW_SERVO_PULSE_CENTER_US;
    s_initialized = true;
    s_last_logged_state = FACE_FOLLOW_STATE_DISABLED;
    s_last_detection_log_ms = 0;
    s_detection_log_initialized = false;
    ESP_LOGI(TAG, "offline face-follow control initialized (disabled)");
    return ESP_OK;
}

esp_err_t app_face_follow_start(void)
{
    if (!s_initialized || s_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Start from a repeatable upper-side pose while leaving headroom for the
     * incremental vertical controller to move in both directions. */
    const int initial_servo_pulse_us = FACE_FOLLOW_START_SERVO_PULSE_US;
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_enabled) {
        xSemaphoreGive(s_mutex);
        return ESP_OK;
    }
    if (s_task != NULL || s_heartbeat_task != NULL) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    while (xSemaphoreTake(s_inference_exit_sem, 0) == pdTRUE) {
    }
    while (xSemaphoreTake(s_heartbeat_exit_sem, 0) == pdTRUE) {
    }
    s_enabled = true;
    s_background_enabled = false;
    s_start_servo_pulse_us = initial_servo_pulse_us;
    s_status.enabled = true;
    s_status.state = FACE_FOLLOW_STATE_STARTING;
    s_status.target = face_target_t{};
    s_status.command = hold_servo_command(
        initial_servo_pulse_us, FACE_FOLLOW_STOP_INVALID_TARGET);
    s_status.last_error = ESP_OK;
    s_last_logged_state = FACE_FOLLOW_STATE_STARTING;
    s_last_detection_log_ms = 0;
    s_detection_log_initialized = false;
    s_previous_sample_ms = 0;
    s_sample_period_ms = 0;

    motion_manager_clear_source(MOTION_SOURCE_FOLLOW);
    app_activity_camera_acquire(APP_ACT_CAMERA_FOLLOW);
    const BaseType_t heartbeat_ret = xTaskCreatePinnedToCoreWithCaps(
        follow_heartbeat_task, "follow_heartbeat", 4096, NULL, 6,
        &s_heartbeat_task, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (heartbeat_ret != pdPASS) {
        s_enabled = false;
        s_background_enabled = false;
        s_status.enabled = false;
        s_status.state = FACE_FOLLOW_STATE_ERROR;
        s_status.last_error = ESP_ERR_NO_MEM;
        xSemaphoreGive(s_mutex);
        app_activity_camera_release(APP_ACT_CAMERA_FOLLOW);
        return ESP_ERR_NO_MEM;
    }

    const BaseType_t task_ret = xTaskCreatePinnedToCoreWithCaps(
        face_follow_task, "face_follow", 12 * 1024, NULL, 5, &s_task, 1,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (task_ret != pdPASS) {
        s_enabled = false;
        s_background_enabled = false;
        s_status.enabled = false;
        s_status.state = FACE_FOLLOW_STATE_ERROR;
        s_status.last_error = ESP_ERR_NO_MEM;
        xSemaphoreGive(s_mutex);
        motion_manager_clear_source(MOTION_SOURCE_FOLLOW);
        (void)xSemaphoreTake(s_heartbeat_exit_sem, pdMS_TO_TICKS(1000));
        app_activity_camera_release(APP_ACT_CAMERA_FOLLOW);
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "offline face-follow enabled: model=MSRMNP_S8_V1 camera_demand=on");
    return ESP_OK;
}

esp_err_t app_face_follow_stop(void)
{
    if (!s_initialized || s_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    TaskHandle_t task = NULL;
    TaskHandle_t heartbeat_task = NULL;
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
        s_enabled = false;
        s_background_enabled = false;
        s_status.enabled = false;
        s_status.state = FACE_FOLLOW_STATE_DISABLED;
        s_status.target = face_target_t{};
        s_status.command = zero_command(FACE_FOLLOW_STOP_INVALID_TARGET);
        task = s_task;
        heartbeat_task = s_heartbeat_task;
        xSemaphoreGive(s_mutex);
    }

    motion_manager_clear_source(MOTION_SOURCE_FOLLOW);
    if (heartbeat_task != NULL &&
        heartbeat_task != xTaskGetCurrentTaskHandle()) {
        (void)xSemaphoreTake(s_heartbeat_exit_sem, pdMS_TO_TICKS(1000));
    }
    if (task != NULL && task != xTaskGetCurrentTaskHandle()) {
        const BaseType_t exited = xSemaphoreTake(
            s_inference_exit_sem, pdMS_TO_TICKS(3500));
        if (exited != pdTRUE) {
            app_activity_camera_release(APP_ACT_CAMERA_FOLLOW);
        }
    } else {
        app_activity_camera_release(APP_ACT_CAMERA_FOLLOW);
    }
    ESP_LOGI(TAG, "offline face-follow disabled: follow source cleared");
    return ESP_OK;
}

bool app_face_follow_is_enabled(void)
{
    return is_enabled();
}

esp_err_t app_face_follow_set_background_enabled(bool enabled)
{
    if (!s_initialized || s_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (enabled && !s_enabled) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    s_background_enabled = enabled;
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "face-follow background mode %s", enabled ? "enabled" : "disabled");
    return ESP_OK;
}

bool app_face_follow_is_background_enabled(void)
{
    if (s_mutex == NULL || xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    const bool enabled = s_enabled && s_background_enabled;
    xSemaphoreGive(s_mutex);
    return enabled;
}

esp_err_t app_face_follow_get_state(face_follow_status_t *out_state)
{
    if (out_state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized || s_mutex == NULL ||
        xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    *out_state = s_status;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}
