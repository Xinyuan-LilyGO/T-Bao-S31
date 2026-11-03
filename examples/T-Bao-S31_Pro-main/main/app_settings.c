/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app_settings.h"

#include <stdint.h>
#include "esp_log.h"
#include "nvs.h"

#define APP_SETTINGS_NAMESPACE "app_settings"
#define APP_SETTINGS_KEY_XIAOZHI "xiaozhi_enabled"
#define APP_SETTINGS_KEY_UI_SOUNDS "ui_sounds_enabled"
#define APP_SETTINGS_KEY_VOLUME  "volume"
#define APP_SETTINGS_KEY_BRIGHT  "brightness"
#define APP_SETTINGS_DEFAULT_VOLUME 80
#define APP_SETTINGS_DEFAULT_BRIGHTNESS 80

static const char *TAG = "APP_SETTINGS";

static void app_settings_set_defaults(app_settings_t *settings)
{
    settings->xiaozhi_enabled = true;
    settings->ui_sounds_enabled = true;
    settings->volume = APP_SETTINGS_DEFAULT_VOLUME;
    settings->brightness = APP_SETTINGS_DEFAULT_BRIGHTNESS;
}

esp_err_t app_settings_load(app_settings_t *settings)
{
    if (settings == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    app_settings_set_defaults(settings);

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(APP_SETTINGS_NAMESPACE, NVS_READONLY, &handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "open settings failed: %s", esp_err_to_name(ret));
        return ret;
    }

    uint8_t enabled = 0;
    if (nvs_get_u8(handle, APP_SETTINGS_KEY_XIAOZHI, &enabled) == ESP_OK) {
        settings->xiaozhi_enabled = enabled != 0;
    }
    enabled = 1;
    if (nvs_get_u8(handle, APP_SETTINGS_KEY_UI_SOUNDS, &enabled) == ESP_OK) {
        settings->ui_sounds_enabled = enabled != 0;
    }

    int32_t value = 0;
    if (nvs_get_i32(handle, APP_SETTINGS_KEY_VOLUME, &value) == ESP_OK &&
        value >= 0 && value <= 100) {
        settings->volume = (int)value;
    }
    if (nvs_get_i32(handle, APP_SETTINGS_KEY_BRIGHT, &value) == ESP_OK &&
        value >= 5 && value <= 100) {
        settings->brightness = (int)value;
    }

    nvs_close(handle);
    ESP_LOGI(TAG, "loaded: xiaozhi=%s ui_sounds=%s volume=%d brightness=%d",
             settings->xiaozhi_enabled ? "on" : "off",
             settings->ui_sounds_enabled ? "on" : "off",
             settings->volume, settings->brightness);
    return ESP_OK;
}

static esp_err_t app_settings_open_write(nvs_handle_t *handle)
{
    esp_err_t ret = nvs_open(APP_SETTINGS_NAMESPACE, NVS_READWRITE, handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "open settings for write failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t app_settings_set_xiaozhi_enabled(bool enabled)
{
    nvs_handle_t handle;
    esp_err_t ret = app_settings_open_write(&handle);
    if (ret == ESP_OK) {
        ret = nvs_set_u8(handle, APP_SETTINGS_KEY_XIAOZHI, enabled ? 1 : 0);
        if (ret == ESP_OK) {
            ret = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "save xiaozhi setting failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t app_settings_set_ui_sounds_enabled(bool enabled)
{
    nvs_handle_t handle;
    esp_err_t ret = app_settings_open_write(&handle);
    if (ret == ESP_OK) {
        ret = nvs_set_u8(handle, APP_SETTINGS_KEY_UI_SOUNDS, enabled ? 1 : 0);
        if (ret == ESP_OK) {
            ret = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "save UI sound setting failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t app_settings_set_volume(int volume)
{
    if (volume < 0 || volume > 100) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t ret = app_settings_open_write(&handle);
    if (ret == ESP_OK) {
        ret = nvs_set_i32(handle, APP_SETTINGS_KEY_VOLUME, volume);
        if (ret == ESP_OK) {
            ret = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "save volume failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t app_settings_set_brightness(int brightness)
{
    if (brightness < 5 || brightness > 100) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t ret = app_settings_open_write(&handle);
    if (ret == ESP_OK) {
        ret = nvs_set_i32(handle, APP_SETTINGS_KEY_BRIGHT, brightness);
        if (ret == ESP_OK) {
            ret = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "save brightness failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t app_settings_reset(void)
{
    nvs_handle_t handle;
    esp_err_t ret = app_settings_open_write(&handle);
    if (ret == ESP_OK) {
        ret = nvs_erase_all(handle);
        if (ret == ESP_OK) {
            ret = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "reset settings failed: %s", esp_err_to_name(ret));
    }
    return ret;
}
