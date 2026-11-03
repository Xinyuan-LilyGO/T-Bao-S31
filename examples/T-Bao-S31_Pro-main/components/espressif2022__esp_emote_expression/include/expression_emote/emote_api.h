/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "emote_init.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Event type constants
 *
 * These constants define the event types used by the emote manager.
 */
#define EMOTE_MGR_EVT_IDLE              "evt_idle"
#define EMOTE_MGR_EVT_SPEAK             "evt_speak"
#define EMOTE_MGR_EVT_LISTEN            "evt_listen"
#define EMOTE_MGR_EVT_SYS               "evt_sys"
#define EMOTE_MGR_EVT_SET               "evt_set"
#define EMOTE_MGR_EVT_BAT               "evt_bat"
#define EMOTE_MGR_EVT_OFF               "evt_off"

/**
 * @brief UI element name constants
 *
 * These are default UI element name constants that define the standard element names
 * used by the emote system. These can be used as default identifiers for UI elements.
 */
#define EMT_DEF_ELEM_DEFAULT_LABEL      "default_label"
#define EMT_DEF_ELEM_EYE_ANIM           "eye_anim"
#define EMT_DEF_ELEM_EMERG_DLG          "emerg_dlg"
#define EMT_DEF_ELEM_TOAST_LABEL        "toast_label"
#define EMT_DEF_ELEM_CLOCK_LABEL        "clock_label"
#define EMT_DEF_ELEM_LISTEN_ANIM        "listen_anim"
#define EMT_DEF_ELEM_STATUS_ICON        "status_icon"
#define EMT_DEF_ELEM_CHARGE_ICON        "charge_icon"
#define EMT_DEF_ELEM_BAT_LEFT_LABEL     "battery_label"
#define EMT_DEF_ELEM_TIMER_STATUS       "clock_timer"
#define EMT_DEF_ELEM_QRCODE             "qrcode"

/**
 * @brief Object type strings
 *
 * These are user-customizable object type strings that define the types of UI objects
 * that can be created and managed by the emote system.
 */
#define EMOTE_OBJ_TYPE_ANIM             "anim"
#define EMOTE_OBJ_TYPE_IMAGE            "image"
#define EMOTE_OBJ_TYPE_LABEL            "label"
#define EMOTE_OBJ_TYPE_QRCODE           "qrcode"
#define EMOTE_OBJ_TYPE_TIMER            "timer"

/**
 * @brief Set emoji animation on eye object
 * @param handle Handle to emote manager
 * @param name Name of the emoji animation
 * @return ESP_OK on success, error code on failure
 */
esp_err_t emote_set_anim_emoji(emote_handle_t handle, const char *name);

/**
 * @brief Set QR code data
 * @param handle Handle to emote manager
 * @param qrcode_text QR code text
 * @return ESP_OK on success, error code on failure
 */
esp_err_t emote_set_qrcode_data(emote_handle_t handle, const char *qrcode_text);

/**
 * @brief Set emergency dialog animation
 * @param handle Handle to emote manager
 * @param name Name of the emoji animation
 * @return ESP_OK on success, error code on failure
 */
esp_err_t emote_set_dialog_anim(emote_handle_t handle, const char *name);

/**
 * @brief Insert emergency dialog animation with auto-stop timer
 * @param handle Handle to emote manager
 * @param name Name of the emoji animation
 * @param duration_ms Duration in milliseconds before auto-stopping
 * @return ESP_OK on success, error code on failure
 */
esp_err_t emote_insert_anim_dialog(emote_handle_t handle, const char *name, uint32_t duration_ms);

/**
 * @brief Stop emergency dialog animation
 * @param handle Handle to emote manager
 * @return ESP_OK on success, error code on failure
 */
esp_err_t emote_stop_anim_dialog(emote_handle_t handle);

/**
 * @brief Wait for emergency dialog animation completion
 * @param handle Handle to emote manager
 * @param timeout_ms Timeout in milliseconds (0 = wait forever)
 * @return ESP_OK on success, ESP_ERR_TIMEOUT on timeout, error code on failure
 */
esp_err_t emote_wait_emerg_dlg_done(emote_handle_t handle, uint32_t timeout_ms);

/**
 * @brief Set system event with message
 * @param handle Handle to emote manager
 * @param event Event type string
 * @param message Message string
 * @return ESP_OK on success, error code on failure
 */
esp_err_t emote_set_event_msg(emote_handle_t handle, const char *event, const char *message);

/**
 * @brief Get graphics object by name
 * @param handle Handle to emote manager
 * @param name Object name (predefined or custom)
 * @return Pointer to gfx_obj_t on success, NULL if object not found
 */
gfx_obj_t *emote_get_obj_by_name(emote_handle_t handle, const char *name);

/**
 * @brief Create object by type string (for custom objects)
 * @param handle Handle to emote manager
 * @param type_str Object type string: "anim", "image"/"img", "label", "qrcode", "timer"
 * @param name Object name (must be unique, not conflict with predefined names)
 * @return Pointer to gfx_obj_t on success, NULL on failure
 */
gfx_obj_t *emote_create_obj_by_type(emote_handle_t handle, const char *type_str, const char *name);

/**
 * @brief Set object visible
 * @param handle Handle to emote manager
 * @param name Object name
 * @param visible True to make object visible, false to hide
 * @return ESP_OK on success, error code on failure
 */
esp_err_t emote_set_obj_visible(emote_handle_t handle, const char *name, bool visible);

/**
 * @brief Set face visible
 * @param handle Handle to emote manager
 * @param visible True to make face visible, false to hide
 * @return ESP_OK on success, error code on failure
 */
esp_err_t emote_set_anim_visible(emote_handle_t handle, bool visible);

/**
 * @brief Lock the emote manager
 * @param handle Handle to emote manager
 * @return ESP_OK on success, error code on failure
 */
esp_err_t emote_lock(emote_handle_t handle);

/**
 * @brief Unlock the emote manager
 * @param handle Handle to emote manager
 * @return ESP_OK on success, error code on failure
 */
esp_err_t emote_unlock(emote_handle_t handle);

/**
 * @brief Notify that flush operation is finished
 * @param handle Handle to emote manager
 * @return ESP_OK on success, error code on failure
 */
esp_err_t emote_notify_flush_finished(emote_handle_t handle);

/**
 * @brief Notify that all refresh operation is finished
 * @param handle Handle to emote manager
 * @return ESP_OK on success, error code on failure
 */
esp_err_t emote_notify_all_refresh(emote_handle_t handle);

/**
 * @brief Get user data
 * @param handle Handle to emote manager
 * @return Pointer to user data on success, NULL on failure
 */
void *emote_get_user_data(emote_handle_t handle);

#ifdef __cplusplus
}
#endif
