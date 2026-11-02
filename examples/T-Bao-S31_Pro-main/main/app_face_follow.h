#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "face_follow_controller.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FACE_FOLLOW_STATE_DISABLED = 0,
    FACE_FOLLOW_STATE_STARTING,
    FACE_FOLLOW_STATE_ACQUIRING,
    FACE_FOLLOW_STATE_FOLLOWING,
    FACE_FOLLOW_STATE_LOST,
    FACE_FOLLOW_STATE_ERROR,
} face_follow_state_t;

typedef struct {
    bool enabled;
    face_follow_state_t state;
    face_target_t target;
    face_follow_command_t command;
    uint32_t last_update_ms;
    esp_err_t last_error;
} face_follow_status_t;

esp_err_t app_face_follow_init(void);
esp_err_t app_face_follow_start(void);
esp_err_t app_face_follow_stop(void);
bool app_face_follow_is_enabled(void);
/** Keep face-follow workers alive when the face-follow page is left. */
esp_err_t app_face_follow_set_background_enabled(bool enabled);
/** True when an enabled face-follow session is allowed to run off-page. */
bool app_face_follow_is_background_enabled(void);
esp_err_t app_face_follow_get_state(face_follow_status_t *out_state);

#ifdef __cplusplus
}
#endif
