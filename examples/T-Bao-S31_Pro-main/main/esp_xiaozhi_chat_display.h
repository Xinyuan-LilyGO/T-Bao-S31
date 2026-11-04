/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/* Forward declaration suffices: this header only returns a pointer to an LVGL
 * object; the full lvgl.h is not required by every TU that includes us. */
typedef struct _lv_obj_t lv_obj_t;

#ifdef __cplusplus
extern "C" {
#endif  /* __cplusplus */

/**
 * @brief  Initialize display UI using board manager's display
 *
 * @note   This function should be called after esp_board_manager_adapter_init() with enable_lvgl=true
 *
 * @return
 *       - ESP_OK   On success
 *       - ESP_ERR_INVALID_STATE   Board manager display not available
 */
esp_err_t esp_xiaozhi_chat_display_init(void);

/**
 * @brief  Get display width in pixels
 *
 * @param[out] width  Pointer to width in pixels
 *
 * @return
 *       - ESP_OK   On success
 *       - ESP_ERR_INVALID_ARG   If width is NULL
 */
esp_err_t esp_xiaozhi_chat_display_get_width(int *width);

/**
 * @brief  Get display height in pixels
 *
 * @param[out] height  Pointer to height in pixels
 *
 * @return
 *       - ESP_OK   On success
 *       - ESP_ERR_INVALID_ARG   If height is NULL
 */
esp_err_t esp_xiaozhi_chat_display_get_height(int *height);

/**
 * @brief  Update LCD status text
 *
 * @param[in] status  Status text to display
 *
 * @return
 *       - ESP_OK   On success
 *       - ESP_ERR_INVALID_STATE   If display not initialized
 */
esp_err_t esp_xiaozhi_chat_display_set_status(const char *status);

/**
 * @brief  Clear the transient status icon and status/notification text
 *
 * @return
 *       - ESP_OK   On success
 *       - ESP_ERR_INVALID_STATE   If display not initialized
 */
esp_err_t esp_xiaozhi_chat_display_clear_status(void);

/**
 * @brief  Update LCD subtitle text (same as status in current UI)
 *
 * @param[in] subtitle  Subtitle text to display
 *
 * @return
 *       - ESP_OK   On success
 *       - ESP_ERR_INVALID_STATE   If display not initialized
 */
esp_err_t esp_xiaozhi_chat_display_set_subtitle(const char *subtitle);

/**
 * @brief  Update LCD volume display / backlight
 *
 * @param[in] volume  Volume value (0-100), used for backlight when supported
 *
 * @return
 *       - ESP_OK   On success
 *       - ESP_ERR_INVALID_STATE   If display not initialized
 */
esp_err_t esp_xiaozhi_chat_display_set_volume(int volume);

/**
 * @brief Update the battery icon and charging state.
 *
 * @param[in] percent  Estimated percentage, or a negative value when invalid
 * @param[in] charging Whether the charger reports an active charge phase
 * @param[in] valid    Whether the sample is valid
 */
esp_err_t esp_xiaozhi_chat_display_set_battery(int percent,
                                               bool charging,
                                               bool valid);

/**
 * @brief  Update LCD backlight brightness
 *
 * @param[in] brightness  Brightness value (0-100)
 *
 * @return
 *       - ESP_OK   On success
 *       - ESP_ERR_INVALID_STATE   If display not initialized
 */
esp_err_t esp_xiaozhi_chat_display_set_brightness(int brightness);

/**
 * @brief  Show notification on display
 *
 * @param[in] notification   Notification text to display
 * @param[in] duration_ms    Duration in milliseconds (default: 3000)
 *
 * @return
 *       - ESP_OK   On success
 *       - ESP_ERR_INVALID_STATE   If display not initialized
 */
esp_err_t esp_xiaozhi_chat_display_set_notification(const char *notification, int duration_ms);

/**
 * @brief Show or update the global low-voltage shutdown countdown.
 *
 * The popup is attached to the display's top layer, independently of pages.
 * Repeated updates preserve user dismissal; hiding the popup does not cancel
 * backend protection. A value of zero displays the final shutdown state.
 */
esp_err_t esp_xiaozhi_chat_display_show_shutdown_countdown(int seconds);

/**
 * @brief Hide the global low-voltage shutdown countdown popup.
 */
esp_err_t esp_xiaozhi_chat_display_hide_shutdown_countdown(void);

/**
 * @brief  Set emotion icon on display
 *
 * @param[in] emotion  Emotion name (e.g., "happy", "sad", "neutral")
 *
 * @return
 *       - ESP_OK   On success
 *       - ESP_ERR_INVALID_STATE   If display not initialized
 */
esp_err_t esp_xiaozhi_chat_display_set_emotion(const char *emotion);

/**
 * @brief  Return whether the animated face is available for idle rotation.
 */
bool esp_xiaozhi_chat_display_emote_is_idle(void);

/**
 * @brief  Select a random animation that differs from the current one.
 *         Works while listening or speaking; callers should gate passive
 *         idle rotation separately.
 *
 * @return
 *       - ESP_OK                 A new animation was selected
 *       - ESP_ERR_INVALID_STATE  Display is not initialized or fewer than two
 *                                animations are available
 */
esp_err_t esp_xiaozhi_chat_display_set_random_emotion(void);

/**
 * @brief  Petting reaction: briefly play the winking emote animation.
 */
void esp_xiaozhi_chat_display_pet(void);

/**
 * @brief  Laugh reaction: briefly play the happy emote animation.
 */
void esp_xiaozhi_chat_display_laugh(void);

/**
 * @brief  Switch the LCD panel from the LVGL provisioning UI to the animated
 *         emote face (mounts the "assets" partition and takes over the panel).
 *
 * @note   Must be called after esp_xiaozhi_chat_display_init(). If emote init or
 *         asset mount fails, the LVGL display is kept as a graceful fallback.
 *
 * @return
 *       - ESP_OK   Emote display is now active
 *       - ESP_FAIL / other   Emote unavailable; LVGL display still in use
 */
esp_err_t esp_xiaozhi_chat_display_enable_emote(void);

/**
 * @brief  Toggle which renderer owns the panel.
 *
 * @param[in] visible  true  -> animated FACE (emote) drives the panel, LVGL
 *                            flushing is stopped; false -> a PAGE (LVGL) drives
 *                            the panel and the emote face is soft-paused.
 *
 * @note   Used by the page manager to switch between the face and page UIs.
 */
esp_err_t esp_xiaozhi_chat_display_set_face_visible(bool visible);

/**
 * @brief  Give the video renderer exclusive ownership of the LCD panel.
 */
esp_err_t esp_xiaozhi_chat_display_set_video_visible(bool visible);

/**
 * @brief  Submit one complete RGB565 video frame while video mode is active.
 */
esp_err_t esp_xiaozhi_chat_display_present_video_frame(const uint16_t *frame,
                                                       int width, int height);

/**
 * @brief  Return the LVGL container that PAGE-mode pages build their widgets
 *         into. NULL if the display is not initialized.
 */
lv_obj_t *esp_xiaozhi_chat_display_page_root(void);

/**
 * @brief  Remove all children from the page-root container (called by the page
 *         manager before a new page builds its UI).
 */
esp_err_t esp_xiaozhi_chat_display_clear_page_root(void);

/**
 * @brief  Set chat message on display
 *
 * @param[in] role     Message role: "user", "assistant", or "system"
 * @param[in] content  Message content
 *
 * @return
 *       - ESP_OK   On success
 *       - ESP_ERR_INVALID_STATE   If display not initialized
 */
esp_err_t esp_xiaozhi_chat_display_set_chat_message(const char *role, const char *content);

/**
 * @brief  Set icon on display
 *
 * @param[in] icon  Icon text or symbol
 *
 * @return
 *       - ESP_OK   On success
 *       - ESP_ERR_INVALID_STATE   If display not initialized
 */
esp_err_t esp_xiaozhi_chat_display_set_icon(const char *icon);

/**
 * @brief  Set image (preview image or other image) on display
 *
 * @param[in] image  Pointer to LVGL image descriptor (lv_img_dsc_t* or lv_image_dsc_t*, can be NULL to hide)
 *
 * @return
 *       - ESP_OK   On success
 *       - ESP_ERR_INVALID_STATE   If display not initialized
 */
esp_err_t esp_xiaozhi_chat_display_set_image(const void *image);

/**
 * @brief  Set display theme
 *
 * @param[in] theme_name  Theme name: "light" or "dark"
 *
 * @return
 *       - ESP_OK   On success
 *       - ESP_ERR_INVALID_STATE   If display not initialized
 */
esp_err_t esp_xiaozhi_chat_display_set_theme(const char *theme_name);

#ifdef __cplusplus
}
#endif  /* __cplusplus */
