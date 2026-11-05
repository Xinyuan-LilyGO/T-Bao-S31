/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_xiaozhi_chat.h"
#include "esp_gmf_oal_thread.h"
#include "freertos/semphr.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif  /* __cplusplus */

#define ESP_XIAOZHI_CHAT_REC_READ_SIZE CONFIG_XIAOZHI_UDP_MAX_AUDIO_PAYLOAD_SIZE
#define ESP_XIAOZHI_CHAT_APP_AUDIO_SEND (1 << 0)
#define ESP_XIAOZHI_CHAT_APP_ONLINE (1 << 1)
#define ESP_XIAOZHI_CHAT_APP_OFFLINE (1 << 2)
#define ESP_XIAOZHI_CHAT_APP_LISTEN_REARM (1 << 3)
#define ESP_XIAOZHI_CHAT_APP_WAKE_INTERRUPT (1 << 4)

/**
 * @brief  ESP IoT Chat App structure
 *
 * @note   This structure is used to store the Xiaozhi chat app data
 */
typedef struct esp_xiaozhi_chat_app_s {
    bool                       wakeuped;       /*!< Wake-up state flag */
    bool                       wake_pending;   /*!< A wake request is queued or opening */
    bool                       wake_interrupt_pending; /*!< Active-session wake is queued */
    bool                       transport_connected; /*!< WebSocket/MQTT transport is ready */
    bool                       tts_playing;   /*!< Server TTS is feeding the speaker */
    bool                       listen_rearm_pending; /*!< Waiting to resume microphone upload */
    bool                       audio_send_blocked; /*!< Audio sender latched after a transport write failure */
    bool                       display_ready; /*!< LVGL/emote ownership is ready for chat UI callbacks */
    int                        audio_send_errors; /*!< Consecutive transient audio send failures */
    EventGroupHandle_t         data_evt_group;  /*!< Data event group */
    SemaphoreHandle_t          state_mutex;    /*!< Serializes wake/transport state */
    esp_xiaozhi_chat_handle_t  chat;           /*!< Xiaozhi chat handle */
    esp_xiaozhi_chat_audio_t   audio;          /*!< Audio configuration */
    esp_gmf_oal_thread_t       read_thread;    /*!< Audio data read thread */
    esp_gmf_oal_thread_t       audio_channel;  /*!< Audio channel thread */
} esp_xiaozhi_chat_app_t;

typedef enum {
    ESP_XIAOZHI_CHAT_APP_STATE_DISABLED = 0,
    ESP_XIAOZHI_CHAT_APP_STATE_STARTING,
    ESP_XIAOZHI_CHAT_APP_STATE_ENABLED,
    ESP_XIAOZHI_CHAT_APP_STATE_STOPPING,
    ESP_XIAOZHI_CHAT_APP_STATE_ERROR,
} esp_xiaozhi_chat_app_state_t;

/**
 * @brief  Run the Xiaozhi chat application
 *
 * @return
 *       - ESP_OK   On success
 *       - ESP_ERR_NO_MEM   Failed to allocate app context
 */
esp_err_t esp_xiaozhi_chat_app(void);

/**
 * @brief Request the Xiaozhi runtime to start or stop in its background worker.
 *
 * The display, page manager, WiFi and board adapter remain alive. Disabling
 * releases chat/MCP and AI audio resources; an active music page keeps the
 * shared speaker output until that page exits.
 */
esp_err_t esp_xiaozhi_chat_app_set_enabled(bool enabled);

/** @brief Ensure the shared speaker path and music decoder are available. */
esp_err_t esp_xiaozhi_chat_app_prepare_music(void);

/** @brief Release the music decoder and idle speaker hardware when possible. */
esp_err_t esp_xiaozhi_chat_app_release_music(void);

/** @brief Queue music/audio release without blocking the UI task. */
esp_err_t esp_xiaozhi_chat_app_release_music_async(void);

/** @brief Return the current runtime lifecycle state. */
esp_xiaozhi_chat_app_state_t esp_xiaozhi_chat_app_get_state(void);

/** @brief Return true only after the chat runtime is fully enabled. */
bool esp_xiaozhi_chat_app_is_enabled(void);

/**
 * @brief  Create the audio-control mutex once (call from app init, single
 *         thread). Guards volume/brightness hardware access across the LVGL
 *         task, MCP task and app task.
 */
void esp_xiaozhi_chat_app_audio_ctrl_lock_init(void);

/**
 * @brief  Set the output volume (0-100). Applies to the codec immediately and
 *         updates the app's current_volume. Used by the control-center slider
 *         and the MCP "set_volume" tool.
 */
esp_err_t esp_xiaozhi_chat_app_set_volume(int volume);

/** @brief Apply volume while a slider moves, without writing NVS. */
esp_err_t esp_xiaozhi_chat_app_preview_volume(int volume);

/** @brief Persist the most recently previewed volume value. */
esp_err_t esp_xiaozhi_chat_app_commit_volume(void);

/**
 * @brief  Set the LCD backlight brightness (0-100).
 */
esp_err_t esp_xiaozhi_chat_app_set_brightness(int brightness);

/** @brief Apply brightness while a slider moves, without writing NVS. */
esp_err_t esp_xiaozhi_chat_app_preview_brightness(int brightness);

/** @brief Persist the most recently previewed brightness value. */
esp_err_t esp_xiaozhi_chat_app_commit_brightness(void);

/**
 * @brief  Current volume / brightness as last set (0-100).
 */
int esp_xiaozhi_chat_app_get_volume(void);
int esp_xiaozhi_chat_app_get_brightness(void);

#ifdef __cplusplus
}
#endif  /* __cplusplus */
