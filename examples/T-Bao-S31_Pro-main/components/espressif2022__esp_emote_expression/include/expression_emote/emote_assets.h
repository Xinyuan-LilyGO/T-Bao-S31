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

typedef struct {
    const void *data;
    size_t size;
} icon_data_t;

typedef struct {
    const void *data;
    size_t size;
    uint8_t fps;
    bool loop;
} emoji_data_t;

/**
 * @brief Mount assets from source
 * @param handle Handle to emote manager
 * @param data Source data structure
 * @return ESP_OK on success, error code on failure
 */
esp_err_t emote_mount_assets(emote_handle_t handle, const emote_data_t *data);

/**
 * @brief Unmount assets
 * @param handle Handle to emote manager
 * @return ESP_OK on success, error code on failure
 */
esp_err_t emote_unmount_assets(emote_handle_t handle);

/**
 * @brief Load assets data (parse JSON and load emojis, icons, layouts, fonts)
 * @param handle Handle to emote manager
 * @return ESP_OK on success, error code on failure
 */
esp_err_t emote_load_assets(emote_handle_t handle);

/**
 * @brief Unload assets data (free emojis, icons, fonts loaded by emote_load_assets)
 * @param handle Handle to emote manager
 * @return ESP_OK on success, error code on failure
 */
esp_err_t emote_unload_assets(emote_handle_t handle);

/**
 * @brief Load assets from source (mount + load data)
 * @param handle Handle to emote manager
 * @param data Source data structure
 * @return ESP_OK on success, error code on failure
 */
esp_err_t emote_mount_and_load_assets(emote_handle_t handle, const emote_data_t *data);

/**
 * @brief Get parsed icon data by name (from parsed icon table)
 * @param handle Handle to emote manager
 * @param name Icon name
 * @param icon Icon data pointer (output parameter)
 * @return ESP_OK on success, error code on failure
 */
esp_err_t emote_get_icon_data_by_name(emote_handle_t handle, const char *name, icon_data_t **icon);

/**
 * @brief Get parsed emoji data by name (from parsed emoji table)
 * @param handle Handle to emote manager
 * @param name Emoji name
 * @param emoji Emoji data pointer (output parameter)
 * @return ESP_OK on success, error code on failure
 */
esp_err_t emote_get_emoji_data_by_name(emote_handle_t handle, const char *name, emoji_data_t **emoji);

/**
 * @brief Get asset file data by name (raw file data from asset bin)
 * @param handle Handle to emote manager
 * @param name Asset file name
 * @param data Pointer to store asset data pointer (output parameter)
 * @param size Pointer to store asset data size (output parameter)
 * @return ESP_OK on success, error code on failure
 */
esp_err_t emote_get_asset_data_by_name(emote_handle_t handle, const char *name, const uint8_t **data, size_t *size);

#ifdef __cplusplus
}
#endif
