/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "emote_defs.h"
#include "esp_err.h"

// Forward declaration for cJSON (only pointer types used in header)
typedef struct cJSON cJSON;

#ifdef __cplusplus
extern "C" {
#endif

// ===== Layout Application Functions =====
/**
 * @brief  Apply font configuration to emote system
 *
 * @param[in]  handle    Emote handle
 * @param[in]  fontData  Font data buffer
 *
 * @return
 *       - ESP_OK  On success
 *       - Other   Error code on failure
 */
esp_err_t emote_apply_fonts(emote_handle_t handle, const uint8_t *fontData);

/**
 * @brief  Apply label layout configuration from JSON
 *
 * @param[in]  handle  Emote handle
 * @param[in]  name    Label element name
 * @param[in]  label   JSON object containing label configuration
 *
 * @return
 *       - ESP_OK  On success
 *       - Other   Error code on failure
 */
esp_err_t emote_apply_label_layout(emote_handle_t handle, const char *name, cJSON *label);

/**
 * @brief  Apply image layout configuration from JSON
 *
 * @param[in]  handle  Emote handle
 * @param[in]  name    Image element name
 * @param[in]  image   JSON object containing image configuration
 *
 * @return
 *       - ESP_OK  On success
 *       - Other   Error code on failure
 */
esp_err_t emote_apply_image_layout(emote_handle_t handle, const char *name, cJSON *image);

/**
 * @brief  Apply timer layout configuration from JSON
 *
 * @param[in]  handle  Emote handle
 * @param[in]  name    Timer element name
 * @param[in]  timer   JSON object containing timer configuration
 *
 * @return
 *       - ESP_OK  On success
 *       - Other   Error code on failure
 */
esp_err_t emote_apply_timer_layout(emote_handle_t handle, const char *name, cJSON *timer);

/**
 * @brief  Apply animation layout configuration from JSON
 *
 * @param[in]  handle     Emote handle
 * @param[in]  name       Animation element name
 * @param[in]  animation  JSON object containing animation configuration
 *
 * @return
 *       - ESP_OK  On success
 *       - Other   Error code on failure
 */
esp_err_t emote_apply_anim_layout(emote_handle_t handle, const char *name, cJSON *animation);

/**
 * @brief  Apply QRCode layout configuration from JSON
 *
 * @param[in]  handle  Emote handle
 * @param[in]  name    QRCode element name
 * @param[in]  qrcode  JSON object containing QRCode configuration
 *
 * @return
 *       - ESP_OK  On success
 *       - Other   Error code on failure
 */
esp_err_t emote_apply_qrcode_layout(emote_handle_t handle, const char *name, cJSON *qrcode);

// ===== UI Operation Functions =====
/**
 * @brief  Update clock label with current time
 *
 * @param[in]  handle  Emote handle
 *
 * @return
 *       - ESP_OK  On success
 *       - Other   Error code on failure
 */
esp_err_t emote_set_label_clock(emote_handle_t handle);

/**
 * @brief  Update battery status display (percentage and charging icon)
 *
 * @param[in]  handle  Emote handle
 *
 * @return
 *       - ESP_OK  On success
 *       - Other   Error code on failure
 */
esp_err_t emote_set_bat_status(emote_handle_t handle);

/**
 * @brief  Update battery percentage label only
 *
 * @param[in]  handle  Emote handle
 *
 * @return
 *       - ESP_OK  On success
 *       - Other   Error code on failure
 */
esp_err_t emote_set_bat_status_label(emote_handle_t handle);

/**
 * @brief  Load charge icon asset and set visibility from handle->bat_is_charging
 *
 * @param[in]  handle  Emote handle
 *
 * @return
 *       - ESP_OK  On success
 *       - Other   Error code on failure
 */
esp_err_t emote_set_bat_charge_icon(emote_handle_t handle);

/**
 * @brief  Set charge icon visible from handle->bat_is_charging only (no asset reload). For status timer on idle UI.
 *
 * @param[in]  handle  Emote handle
 */
void emote_set_bat_status_charge(emote_handle_t handle);

/**
 * @brief  Create object by name
 *
 * @param[in]  handle  Emote handle
 * @param[in]  name    Object name
 *
 * @return
 *       - Pointer to object  On success
 *       - NULL               Fail to create object
 */
gfx_obj_t *emote_create_obj_by_name(emote_handle_t handle, const char *name);

#ifdef __cplusplus
}
#endif
