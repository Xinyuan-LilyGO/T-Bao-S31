/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "audio_processor.h"
#include "esp_gmf_afe.h"
#include "esp_gmf_oal_sys.h"
#include "esp_gmf_oal_thread.h"
#include "esp_gmf_oal_mem.h"
#include "esp_codec_dev.h"
#include "esp_board_manager_adapter.h"
#include "esp_board_manager_includes.h"

#include "esp_mcp_engine.h"
#include "esp_mcp_tool.h"
#include "esp_mcp_property.h"
#include "esp_mcp_data.h"
#include "esp_xiaozhi_camera.h"
#include "esp_xiaozhi_info.h"
#include "esp_xiaozhi_chat_app.h"
#include "esp_xiaozhi_chat_display.h"
#include "wifi_provisioning.h"
#include "app_audio_dsp.h"
#include "tbao_s31_board.h"
#include "battery_monitor.h"
#include "motion_behavior.h"
#include "motion_mcp.h"
#include "motion_manager.h"
#include "motion_web_server.h"
#include "app_settings.h"
#include "app_language.h"

#include "app_chat_history.h"
#include "app_sfx.h"
#include "app_music_player.h"
#include "app_activity.h"
#include "app_face_follow.h"
#include "app_sdcard.h"
#include "boot_splash.h"
#include "ui_touch.h"
#include "ui_page_manager.h"
#include "ui_control_center.h"
#include "page_face.h"
#include "page_launcher.h"
#include "page_dialogue.h"
#include "page_camera.h"
#include "page_music.h"
#include "page_video.h"
#include "page_game.h"
#include "page_timer.h"
#include "page_emotion.h"
#include "page_settings.h"
#include "page_xiaozhi.h"
#include "page_motion.h"
#include "page_face_follow.h"

#if CONFIG_TBAO_CAMERA_SUPPORT
#include "app_camera.h"
#endif

/* The camera take_photo tool authenticates with the xiaozhi vision service using
 * the device's own auth token (stored in the NVS keystore namespace "websocket",
 * key "token"). The keystore API is private to the esp_xiaozhi component, so its
 * header is exposed to main via main/CMakeLists.txt INCLUDE_DIRS. */
#include "esp_xiaozhi_keystore.h"

/* Page table consumed by the page manager. Stored as pointers because the page
 * structs live in other translation units. */
static const ui_page_t *s_pages[PAGE_MAX] = {
    [PAGE_FACE]     = &page_face,
    [PAGE_LAUNCHER] = &page_launcher,
    [PAGE_DIALOGUE] = &page_dialogue,
    [PAGE_CAMERA]   = &page_camera,
    [PAGE_MUSIC]    = &page_music,
    [PAGE_VIDEO]    = &page_video,
    [PAGE_GAME]     = &page_game,
    [PAGE_TIMER]    = &page_timer,
    [PAGE_EMOTION]  = &page_emotion,
    [PAGE_SETTINGS] = &page_settings,
    [PAGE_XIAOZHI]  = &page_xiaozhi,
    [PAGE_MOTION]   = &page_motion,
    [PAGE_FACE_FOLLOW] = &page_face_follow,
};

static char *TAG = "ESP_XIAOZHI_CHAT_APP";

#define ESP_XIAOZHI_CHAT_APP_AUDIO_READ_ERROR_BACKOFF_MS 20
#define ESP_XIAOZHI_CHAT_APP_AUDIO_READ_ERROR_LOG_INTERVAL 50
#define ESP_XIAOZHI_CHAT_APP_AUDIO_READ_MAX_ERRORS 200
#define XIAOZHI_WAKE_RETRY_INITIAL_MS 2000U
#define XIAOZHI_WAKE_RETRY_MAX_MS 10000U

/* Keep the WebSocket event task free of the blocking audio FIFO. The protocol
 * can deliver a short burst of compressed frames faster than the decoder can
 * consume them, so keep a bounded PSRAM backlog and never block the protocol
 * callback. */
#define XIAOZHI_TTS_FRAME_MAX_BYTES 2048U
#define XIAOZHI_TTS_QUEUE_DEPTH 96U
#define XIAOZHI_TTS_STOP_SLOT 0xFFU
#define XIAOZHI_TTS_CMD_RESET_FEEDER 0xFEU
#define XIAOZHI_TTS_FEED_TASK_STACK (4 * 1024)
#define XIAOZHI_TTS_FEED_TASK_PRIORITY 11
#define XIAOZHI_TTS_CONTROL_POLL_MS 20U
#define XIAOZHI_TTS_DRAIN_POLL_MS 20U
#define XIAOZHI_TTS_DRAIN_TIMEOUT_MS 3000U

/* Keep realtime microphone audio bounded like the official AudioService. The
 * recorder must never wait for the network; stale frames have no value once
 * the send path falls behind. Store the bulk payloads in PSRAM while keeping
 * the queue control and the task stacks in internal RAM. */
#define XIAOZHI_AUDIO_SEND_QUEUE_DEPTH (2400 / 60)

typedef struct {
    QueueHandle_t free_slots;
    QueueHandle_t ready_slots;
    uint8_t *storage;
    uint16_t lengths[XIAOZHI_AUDIO_SEND_QUEUE_DEPTH];
    uint32_t dropped_frames;
} xiaozhi_audio_send_runtime_t;

/* AFE input channel layout.
 *
 * The board adapter derives "MR" (Mic + AEC Reference) from adc_channel_mask
 * 0b0011, which is correct for Korvo-style boards that loop the speaker signal
 * back into ADC channel 1. The VoCat-S31 does NOT: the ES8389 records two real
 * differential microphones (MIC1P/N and MIC2P/N, adc_channel_mask 0x3, labelled
 * "FL,FR"), and there is NO hardware AEC reference loop.
 *
 * "MM" = two real microphones, no reference. esp-sr then enables dual-mic
 * beamforming (BF) instead of AEC, which is exactly what this hardware can use
 * to suppress ambient noise and reject the on-board speaker during barge-in.
 * We fall back to "MN" (single mic, channel 1 unused) only if the board reports
 * fewer than 2 capture channels, so a misconfigured board still boots. */
#define ESP_XIAOZHI_CHAT_APP_MIC_LAYOUT_DUAL "MM"
#define ESP_XIAOZHI_CHAT_APP_MIC_LAYOUT_SINGLE "MN"

/* Capture a short block straight from the codec at start-up and log its level.
 * This is the only reliable way to tell "the mic hardware is silent" apart from
 * "the wake word engine is misconfigured". */
#define ESP_XIAOZHI_CHAT_APP_MIC_SELFTEST_BLOCKS 3

static int current_volume = 80;   /* default out volume: 0-100 -> -50..0 dB (80 ~ -10 dB, balanced) */
static int current_brightness = 80;
static char current_theme[32] = "light";
static int current_hue = 0;          // 0..360
static int current_saturation = 0;   // 0..100
static int current_value = 0;        // 0..100
static int current_red = 0;          // 0..255
static int current_green = 0;        // 0..255
static int current_blue = 0;         // 0..255
static app_settings_t s_settings = {
    .xiaozhi_enabled = true,
    .volume = 80,
    .brightness = 80,
};
static esp_xiaozhi_camera_handle_t *s_camera_explain = NULL;
static esp_codec_dev_handle_t s_play_dev = NULL;

#define XIAOZHI_RUNTIME_TASK_STACK (8 * 1024)

typedef struct {
    bool audio_manager_inited;
    bool play_dev_opened;
    bool rec_dev_opened;
    bool playback_opened;
    bool recorder_opened;
    bool feeder_opened;
    bool feeder_running;
    audio_feeder_config_t feeder_config;
    bool dsp_inited;
    bool music_player_inited;
} xiaozhi_audio_runtime_t;

typedef struct {
    QueueHandle_t free_slots;
    QueueHandle_t ready_slots;
    QueueHandle_t control_slots;
    SemaphoreHandle_t exited;
    uint8_t *storage;
    uint16_t lengths[XIAOZHI_TTS_QUEUE_DEPTH];
    TaskHandle_t task;
    volatile bool stop_requested;
    volatile bool feeding;
    volatile bool discard_until_start;
} xiaozhi_tts_runtime_t;

static esp_xiaozhi_chat_app_t *s_xiaozhi_chat_app = NULL;
static esp_board_manager_adapter_info_t s_bsp_info = {0};
static xiaozhi_audio_runtime_t s_audio_runtime = {0};
/* AFE element used to gate PCM output while the server TTS is playing. The
 * AFE feed/fetch and WakeNet/VAD stay active; only the recorder feed stops. */
static esp_gmf_element_handle_t s_afe_element = NULL;

static volatile bool s_afe_voice_output = true;
static volatile bool s_log_first_audio_frame = false;

/* Upstream (CONFIG_SEND_WAKE_WORD_DATA) streams ~2 s of Opus audio captured
 * around the wake word before the listen/detect message, so the server can run
 * voice-print verification and keep the dialogue session open for the turns
 * that follow the greeting. The read thread keeps a rolling window of encoded
 * frames while idle; the window is frozen when a wake becomes pending and is
 * flushed once the audio channel handshake completes. */
#define XIAOZHI_PREROLL_FRAME_COUNT 50U /* ~3 s at 60 ms/frame */
static uint8_t *s_preroll_storage;
static uint16_t s_preroll_lengths[XIAOZHI_PREROLL_FRAME_COUNT];
static uint16_t s_preroll_head;
static uint16_t s_preroll_count;
static SemaphoreHandle_t s_preroll_mutex;

static bool xiaozhi_preroll_init(void)
{
    if (s_preroll_storage != NULL && s_preroll_mutex != NULL) {
        return true;
    }
    if (s_preroll_storage == NULL) {
        s_preroll_storage = heap_caps_calloc(
            XIAOZHI_PREROLL_FRAME_COUNT, ESP_XIAOZHI_CHAT_REC_READ_SIZE,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_preroll_storage == NULL) {
            return false;
        }
    }
    if (s_preroll_mutex == NULL) {
        s_preroll_mutex = xSemaphoreCreateMutex();
        if (s_preroll_mutex == NULL) {
            heap_caps_free(s_preroll_storage);
            s_preroll_storage = NULL;
            return false;
        }
    }
    s_preroll_head = 0;
    s_preroll_count = 0;
    return true;
}

static void xiaozhi_preroll_capture(esp_xiaozhi_chat_app_t *app,
                                    const uint8_t *data, size_t len)
{
    if (app == NULL || data == NULL || len == 0 ||
            len > ESP_XIAOZHI_CHAT_REC_READ_SIZE ||
            s_preroll_storage == NULL || s_preroll_mutex == NULL) {
        return;
    }
    bool frozen = false;
    if (app->state_mutex != NULL &&
            xSemaphoreTake(app->state_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        frozen = app->wake_pending || app->wakeuped;
        xSemaphoreGive(app->state_mutex);
    }
    if (frozen || xSemaphoreTake(s_preroll_mutex, 0) != pdTRUE) {
        return;
    }
    const uint16_t slot = s_preroll_head;
    memcpy(s_preroll_storage + (size_t)slot * ESP_XIAOZHI_CHAT_REC_READ_SIZE,
           data, len);
    s_preroll_lengths[slot] = (uint16_t)len;
    s_preroll_head = (uint16_t)((slot + 1U) % XIAOZHI_PREROLL_FRAME_COUNT);
    if (s_preroll_count < XIAOZHI_PREROLL_FRAME_COUNT) {
        ++s_preroll_count;
    }
    xSemaphoreGive(s_preroll_mutex);
}

static void xiaozhi_preroll_flush(esp_xiaozhi_chat_app_t *app)
{
    if (app == NULL || app->chat == 0 || s_preroll_storage == NULL ||
            s_preroll_mutex == NULL) {
        return;
    }
    xSemaphoreTake(s_preroll_mutex, portMAX_DELAY);
    const uint16_t count = s_preroll_count;
    const uint16_t start = (uint16_t)((s_preroll_head + XIAOZHI_PREROLL_FRAME_COUNT -
                                       count) % XIAOZHI_PREROLL_FRAME_COUNT);
    uint16_t sent = 0;
    for (uint16_t i = 0; i < count; ++i) {
        const uint16_t idx =
            (uint16_t)((start + i) % XIAOZHI_PREROLL_FRAME_COUNT);
        const uint16_t len = s_preroll_lengths[idx];
        if (len == 0) {
            continue;
        }
        const esp_err_t ret = esp_xiaozhi_chat_send_audio_data(
            app->chat,
            (const char *)(s_preroll_storage +
                           (size_t)idx * ESP_XIAOZHI_CHAT_REC_READ_SIZE),
            len);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "wake preroll send failed (%u/%u): %s",
                     (unsigned)i, (unsigned)count, esp_err_to_name(ret));
            break;
        }
        ++sent;
    }
    s_preroll_count = 0;
    s_preroll_head = 0;
    xSemaphoreGive(s_preroll_mutex);
    if (count > 0) {
        ESP_LOGI(TAG, "wake preroll sent (%u/%u frames)", (unsigned)sent,
                 (unsigned)count);
    }
}

static void xiaozhi_preroll_deinit(void)
{
    if (s_preroll_mutex != NULL) {
        vSemaphoreDelete(s_preroll_mutex);
        s_preroll_mutex = NULL;
    }
    if (s_preroll_storage != NULL) {
        heap_caps_free(s_preroll_storage);
        s_preroll_storage = NULL;
    }
    s_preroll_head = 0;
    s_preroll_count = 0;
}

static void xiaozhi_afe_set_voice_output(bool enabled)
{
    if (s_afe_voice_output == enabled) {
        return;
    }
    s_afe_voice_output = enabled;
    if (s_afe_element != NULL) {
        (void)esp_gmf_afe_enable_voice_output(s_afe_element, enabled);
    }
}

/* Keep the AFE wake state alive for the whole conversation. The GMF AFE
 * wrapper disables its VAD and emits WAKEUP_END when the wakeup/wait-for-sleep
 * timer expires (a few seconds of silence, e.g. during a long TTS answer).
 * After that local VAD events stop and the turn handling falls apart. The
 * upstream client keeps voice processing (and therefore VAD) enabled while
 * listening; keep_awake is the equivalent for this wrapper. */
static void xiaozhi_afe_set_keep_awake(bool enabled)
{
    if (s_afe_element != NULL) {
        (void)esp_gmf_afe_keep_awake(s_afe_element, enabled);
    }
}
static xiaozhi_tts_runtime_t s_tts_runtime = {0};
static xiaozhi_audio_send_runtime_t s_audio_send_runtime = {0};
static TaskHandle_t s_runtime_task = NULL;
static volatile bool s_runtime_requested = true;
static volatile esp_xiaozhi_chat_app_state_t s_runtime_state =
    ESP_XIAOZHI_CHAT_APP_STATE_DISABLED;
static volatile esp_err_t s_runtime_last_error = ESP_OK;

/* The MCP tool callback runs in the small (4 KB) websocket_task stack. The
 * camera capture + TLS-explain path overflows that stack (Guru Meditation /
 * Stack protection fault). Offload the heavy work to a dedicated worker task
 * with a large stack; the callback just posts the job and blocks on a
 * semaphore, so the websocket_task never grows its stack. */
#define CAM_WORKER_STACK (24 * 1024)

typedef struct {
    const esp_mcp_property_list_t *properties;
    esp_mcp_value_t result;
} cam_job_t;

static TaskHandle_t s_cam_worker = NULL;
static SemaphoreHandle_t s_cam_done_sem = NULL;   /* worker -> caller */
static SemaphoreHandle_t s_cam_call_mutex = NULL; /* serialize calls */
static cam_job_t s_cam_job;

static esp_mcp_value_t camera_take_photo_do(const esp_mcp_property_list_t *properties);
static esp_err_t esp_xiaozhi_chat_app_init(esp_xiaozhi_chat_app_t *xiaozhi_chat_app);
static esp_err_t esp_xiaozhi_chat_app_audio(esp_xiaozhi_chat_app_t *xiaozhi_chat_app,
                                            esp_board_manager_adapter_info_t bsp_info);
static esp_err_t xiaozhi_audio_release(esp_xiaozhi_chat_app_t *xiaozhi_chat_app,
                                       const esp_board_manager_adapter_info_t *bsp_info);
static esp_err_t xiaozhi_runtime_enable(void);
static esp_err_t xiaozhi_runtime_disable(void);
static void xiaozhi_runtime_task(void *arg);
static bool xiaozhi_chat_app_queue_wakeup(esp_xiaozhi_chat_app_t *xiaozhi_chat_app);
static bool xiaozhi_chat_app_queue_active_wakeup(
    esp_xiaozhi_chat_app_t *xiaozhi_chat_app);
static void xiaozhi_chat_app_set_display_ready(
    esp_xiaozhi_chat_app_t *xiaozhi_chat_app, bool ready);
static void xiaozhi_chat_app_set_wakeuped(esp_xiaozhi_chat_app_t *xiaozhi_chat_app,
                                          bool wakeuped);
static void xiaozhi_chat_app_set_transport_connected(
    esp_xiaozhi_chat_app_t *xiaozhi_chat_app, bool connected);
static void xiaozhi_chat_app_set_audio_send_blocked(
    esp_xiaozhi_chat_app_t *xiaozhi_chat_app, bool blocked);
static bool xiaozhi_chat_app_is_audio_send_blocked(
    esp_xiaozhi_chat_app_t *xiaozhi_chat_app);
static void xiaozhi_tts_feed_shutdown(void);
static void xiaozhi_chat_app_set_tts_playing(
    esp_xiaozhi_chat_app_t *xiaozhi_chat_app, bool playing);
static bool xiaozhi_chat_app_is_tts_playing(
    esp_xiaozhi_chat_app_t *xiaozhi_chat_app);
static bool xiaozhi_chat_app_is_listen_rearm_pending(
    esp_xiaozhi_chat_app_t *xiaozhi_chat_app);
static bool xiaozhi_chat_app_is_wakeuped(
    esp_xiaozhi_chat_app_t *xiaozhi_chat_app);
static bool xiaozhi_chat_app_request_listen_rearm(
    esp_xiaozhi_chat_app_t *xiaozhi_chat_app);
static bool xiaozhi_chat_app_force_listen_rearm(
    esp_xiaozhi_chat_app_t *xiaozhi_chat_app);
static void xiaozhi_tts_wait_for_drain(void);
static void xiaozhi_tts_feed_discard(void);
static void xiaozhi_audio_send_clear(void);
static void xiaozhi_chat_app_handle_wake_interrupt(
    esp_xiaozhi_chat_app_t *xiaozhi_chat_app);

static void battery_monitor_display_callback(
    const battery_monitor_state_t *state,
    void *context)
{
    (void)context;
    if (state != NULL) {
        (void)esp_xiaozhi_chat_display_set_battery(
            state->battery_percent, state->charging, state->valid);
    }
}

static void camera_worker_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        /* wait for a job */
        if (s_cam_worker == NULL) {
            continue;
        }
        s_cam_job.result = camera_take_photo_do(s_cam_job.properties);
        xSemaphoreGive(s_cam_done_sem);
    }
}

static void camera_worker_init(void)
{
    if (s_cam_worker != NULL) {
        return;
    }
    s_cam_done_sem = xSemaphoreCreateBinary();
    s_cam_call_mutex = xSemaphoreCreateMutex();
    if (s_cam_done_sem == NULL || s_cam_call_mutex == NULL) {
        ESP_LOGE(TAG, "camera worker sem init failed");
        return;
    }
    /* The 24 KB worker stack must come from PSRAM, NOT internal RAM: with the
     * camera's ~5.5 MB of PSRAM buffers live the board has only ~32 KB of
     * internal RAM free, and xTaskCreate (internal RAM) used to exhaust it,
     * making subsequent allocations fail (page switches / MCP spawn crashes). */
    BaseType_t r = xTaskCreatePinnedToCoreWithCaps(camera_worker_task, "cam_worker",
                                                   CAM_WORKER_STACK, NULL, 4,
                                                   &s_cam_worker, 0,
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (r != pdPASS) {
        ESP_LOGE(TAG, "camera worker task create failed");
        s_cam_worker = NULL;
        return;
    }
    ESP_LOGI(TAG, "camera worker task started (stack=%u B, PSRAM) — take_photo offloaded off websocket_task",
             (unsigned)CAM_WORKER_STACK);
}

static esp_err_t init_camera_explain_if_needed(void)
{
    if (s_camera_explain != NULL) {
        return ESP_OK;
    }

    esp_xiaozhi_camera_config_t camera_config = {
        .explain_url = NULL,
        .explain_token = NULL,
    };
    return esp_xiaozhi_camera_create(&camera_config, &s_camera_explain);
}

/* Only the TTS worker calls this helper, serializing feeder lifecycle changes
 * with writes to the feeder FIFO. Unlike the upstream AudioService, this
 * audio_processor API does not expose an in-place Opus decoder reset. Its stop
 * path aborts the FIFO, so recovery starts a new feeder generation instead of
 * trying to reuse the stopped GMF task. */
static esp_err_t xiaozhi_tts_recover_feeder(void)
{
    if (s_tts_runtime.stop_requested) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = ESP_OK;
    s_audio_runtime.feeder_running = false;
    if (s_audio_runtime.feeder_opened) {
        ret = audio_feeder_close();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to close feeder for TTS recovery: %s",
                     esp_err_to_name(ret));
            return ret;
        }
        s_audio_runtime.feeder_opened = false;
    }

    /* Shutdown may be requested while close waits for the GMF worker. In that
     * case leave the singleton closed; audio_release will continue with the
     * recorder/playback teardown without reopening resources. */
    if (s_tts_runtime.stop_requested) {
        return ESP_ERR_INVALID_STATE;
    }

    ret = audio_feeder_open(&s_audio_runtime.feeder_config);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open a new feeder generation: %s",
                 esp_err_to_name(ret));
        return ret;
    }
    s_audio_runtime.feeder_opened = true;

    if (s_tts_runtime.stop_requested) {
        ret = audio_feeder_close();
        if (ret == ESP_OK) {
            s_audio_runtime.feeder_opened = false;
        } else {
            ESP_LOGW(TAG, "Failed to close feeder during TTS shutdown: %s",
                     esp_err_to_name(ret));
        }
        return ESP_ERR_INVALID_STATE;
    }

    ret = audio_feeder_run();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to run new feeder generation: %s",
                 esp_err_to_name(ret));
        esp_err_t close_ret = audio_feeder_close();
        if (close_ret == ESP_OK) {
            s_audio_runtime.feeder_opened = false;
        } else {
            ESP_LOGW(TAG, "Failed to close feeder after run failure: %s",
                     esp_err_to_name(close_ret));
        }
        return ret;
    }
    s_audio_runtime.feeder_running = true;
    return ESP_OK;
}

static void xiaozhi_tts_drop_queued_frames(void)
{
    if (s_tts_runtime.ready_slots == NULL ||
            s_tts_runtime.free_slots == NULL) {
        return;
    }

    uint8_t stale_slot;
    while (xQueueReceive(s_tts_runtime.ready_slots, &stale_slot, 0) == pdTRUE) {
        if (stale_slot < XIAOZHI_TTS_QUEUE_DEPTH) {
            xQueueSend(s_tts_runtime.free_slots, &stale_slot, 0);
        }
    }
}

static void xiaozhi_tts_feed_task(void *arg)
{
    (void)arg;
    uint8_t slot = XIAOZHI_TTS_STOP_SLOT;

    while (!s_tts_runtime.stop_requested) {
        uint8_t command;
        if (xQueueReceive(s_tts_runtime.control_slots, &command, 0) == pdTRUE) {
            if (command == XIAOZHI_TTS_CMD_RESET_FEEDER) {
                (void)xiaozhi_tts_recover_feeder();
            }
            continue;
        }

        if (xQueueReceive(s_tts_runtime.ready_slots, &slot,
                          pdMS_TO_TICKS(XIAOZHI_TTS_CONTROL_POLL_MS)) != pdTRUE) {
            continue;
        }
        if (slot == XIAOZHI_TTS_STOP_SLOT) {
            break;
        }
        if (slot >= XIAOZHI_TTS_QUEUE_DEPTH) {
            ESP_LOGW(TAG, "Ignoring invalid TTS frame slot %u", (unsigned)slot);
            continue;
        }

        if (!s_audio_runtime.feeder_opened || !s_audio_runtime.feeder_running) {
            esp_err_t recovery_ret = xiaozhi_tts_recover_feeder();
            if (recovery_ret != ESP_OK) {
                ESP_LOGW(TAG, "Dropping TTS frame; playback feeder is unavailable: %s",
                         esp_err_to_name(recovery_ret));
                s_tts_runtime.discard_until_start = true;
                xiaozhi_tts_drop_queued_frames();
                xQueueSend(s_tts_runtime.free_slots, &slot, 0);
                continue;
            }
        }

        uint8_t *frame = s_tts_runtime.storage +
                         ((size_t)slot * XIAOZHI_TTS_FRAME_MAX_BYTES);
        s_tts_runtime.feeding = true;
        esp_err_t feed_ret = audio_feeder_feed_data(
            frame, s_tts_runtime.lengths[slot]);
        s_tts_runtime.feeding = false;
        if (feed_ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to feed TTS frame: %s",
                     esp_err_to_name(feed_ret));
            if (feed_ret != ESP_ERR_TIMEOUT) {
                /* An actual feeder/FIFO failure is a broken playback
                 * generation: flush its queued packets and reset the decoder
                 * pipeline before accepting the next TTS turn. A bounded
                 * timeout only drops this frame; later Opus packets may still
                 * decode and play, so do not silence the entire answer. */
                s_tts_runtime.discard_until_start = true;
                xiaozhi_tts_drop_queued_frames();
                (void)xiaozhi_tts_recover_feeder();
            }
        }
        xQueueSend(s_tts_runtime.free_slots, &slot, portMAX_DELAY);
    }

    if (s_tts_runtime.exited != NULL) {
        xSemaphoreGive(s_tts_runtime.exited);
    }
    /* Created with xTaskCreatePinnedToCoreWithCaps (PSRAM or internal); the
     * IDF kernel does not reclaim a WithCaps stack through plain vTaskDelete,
     * so each create/delete cycle would leak the stack. */
    vTaskDeleteWithCaps(NULL);
}

static esp_err_t xiaozhi_tts_feed_init(void)
{
    if (s_tts_runtime.task != NULL) {
        return ESP_OK;
    }

    memset(&s_tts_runtime, 0, sizeof(s_tts_runtime));
    const size_t storage_size = XIAOZHI_TTS_QUEUE_DEPTH * XIAOZHI_TTS_FRAME_MAX_BYTES;
    s_tts_runtime.storage = heap_caps_calloc(1, storage_size,
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_tts_runtime.storage == NULL) {
        ESP_LOGE(TAG, "Failed to allocate TTS frame queue (%u bytes)",
                 (unsigned)storage_size);
        return ESP_ERR_NO_MEM;
    }

    s_tts_runtime.free_slots = xQueueCreate(XIAOZHI_TTS_QUEUE_DEPTH, sizeof(uint8_t));
    s_tts_runtime.ready_slots = xQueueCreate(XIAOZHI_TTS_QUEUE_DEPTH, sizeof(uint8_t));
    s_tts_runtime.control_slots = xQueueCreate(1, sizeof(uint8_t));
    s_tts_runtime.exited = xSemaphoreCreateBinary();
    if (s_tts_runtime.free_slots == NULL || s_tts_runtime.ready_slots == NULL ||
            s_tts_runtime.control_slots == NULL || s_tts_runtime.exited == NULL) {
        ESP_LOGE(TAG, "Failed to create TTS frame queues");
        if (s_tts_runtime.free_slots != NULL) {
            vQueueDelete(s_tts_runtime.free_slots);
        }
        if (s_tts_runtime.ready_slots != NULL) {
            vQueueDelete(s_tts_runtime.ready_slots);
        }
        if (s_tts_runtime.control_slots != NULL) {
            vQueueDelete(s_tts_runtime.control_slots);
        }
        if (s_tts_runtime.exited != NULL) {
            vSemaphoreDelete(s_tts_runtime.exited);
        }
        heap_caps_free(s_tts_runtime.storage);
        memset(&s_tts_runtime, 0, sizeof(s_tts_runtime));
        return ESP_ERR_NO_MEM;
    }

    for (uint8_t slot = 0; slot < XIAOZHI_TTS_QUEUE_DEPTH; ++slot) {
        xQueueSend(s_tts_runtime.free_slots, &slot, 0);
    }

    /* The feed worker only touches the playback FIFO and decoder; it performs
     * no flash/NVS access, so its stack can live in PSRAM. Internal SRAM is
     * needed by the WebSocket receive worker (MCP callbacks may write NVS)
     * and by TLS/DMA, and was dropping to ~4 KiB after a few conversations. */
    BaseType_t task_ret = xTaskCreatePinnedToCoreWithCaps(
        xiaozhi_tts_feed_task, "xiaozhi_tts", XIAOZHI_TTS_FEED_TASK_STACK,
        NULL, XIAOZHI_TTS_FEED_TASK_PRIORITY, &s_tts_runtime.task, 1,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (task_ret != pdPASS) {
        ESP_LOGW(TAG, "TTS feed PSRAM stack unavailable; retrying internal");
        task_ret = xTaskCreatePinnedToCoreWithCaps(
            xiaozhi_tts_feed_task, "xiaozhi_tts", XIAOZHI_TTS_FEED_TASK_STACK,
            NULL, XIAOZHI_TTS_FEED_TASK_PRIORITY, &s_tts_runtime.task, 1,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create TTS feed task");
        vQueueDelete(s_tts_runtime.free_slots);
        vQueueDelete(s_tts_runtime.ready_slots);
        vQueueDelete(s_tts_runtime.control_slots);
        vSemaphoreDelete(s_tts_runtime.exited);
        heap_caps_free(s_tts_runtime.storage);
        memset(&s_tts_runtime, 0, sizeof(s_tts_runtime));
        return ESP_ERR_NO_MEM;
    }

    s_tts_runtime.stop_requested = false;
    s_tts_runtime.feeding = false;
    ESP_LOGI(TAG, "TTS feed queue started (depth=%u, frame=%u bytes)",
             (unsigned)XIAOZHI_TTS_QUEUE_DEPTH,
             (unsigned)XIAOZHI_TTS_FRAME_MAX_BYTES);
    return ESP_OK;
}

static void xiaozhi_tts_feed_shutdown(void)
{
    if (s_tts_runtime.task == NULL) {
        return;
    }

    s_tts_runtime.stop_requested = true;
    /* Do not stop or destroy the feeder while a FIFO write may be in flight.
     * Feed operations have bounded waits; let the owning task finish, then
     * audio_release stops and closes the feeder below. */
    if (s_tts_runtime.exited != NULL) {
        xSemaphoreTake(s_tts_runtime.exited, portMAX_DELAY);
    }
    s_tts_runtime.task = NULL;

    vQueueDelete(s_tts_runtime.free_slots);
    vQueueDelete(s_tts_runtime.ready_slots);
    vQueueDelete(s_tts_runtime.control_slots);
    vSemaphoreDelete(s_tts_runtime.exited);
    heap_caps_free(s_tts_runtime.storage);
    memset(&s_tts_runtime, 0, sizeof(s_tts_runtime));
}

static bool xiaozhi_tts_is_idle(void)
{
    if (s_tts_runtime.ready_slots == NULL) {
        return true;
    }
    return !s_tts_runtime.feeding &&
           uxQueueMessagesWaiting(s_tts_runtime.ready_slots) == 0;
}

static void xiaozhi_tts_wait_for_drain(void)
{
    const TickType_t deadline = xTaskGetTickCount() +
                                pdMS_TO_TICKS(XIAOZHI_TTS_DRAIN_TIMEOUT_MS);
    while (!xiaozhi_tts_is_idle()) {
        const TickType_t now = xTaskGetTickCount();
        if ((int32_t)(deadline - now) <= 0) {
            ESP_LOGW(TAG, "TTS feed queue did not drain before listen rearm");
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(XIAOZHI_TTS_DRAIN_POLL_MS));
    }

    /* The compressed-frame worker can finish before the decoder FIFO and codec
     * output have drained. Ask the feeder for a condition-based drain instead
     * of guessing with a fixed delay. The feeder derives its quiet window from
     * the actual PCM output block size and board sample format. */
    const esp_err_t drain_ret = audio_feeder_wait_for_drain(
        XIAOZHI_TTS_DRAIN_TIMEOUT_MS);
    if (drain_ret != ESP_OK) {
        ESP_LOGW(TAG, "Audio feeder did not drain before listen rearm: %s",
                 esp_err_to_name(drain_ret));
    }
}

/* Interrupting a spoken answer is a generation change, not a normal queue
 * drain. Drop both the decoder FIFO and any compressed frames that arrived
 * after the wake word, then resume the feeder for the next answer. */
static void xiaozhi_tts_feed_discard(void)
{
    if (s_tts_runtime.ready_slots == NULL ||
            s_tts_runtime.free_slots == NULL ||
            s_tts_runtime.control_slots == NULL) {
        return;
    }

    s_tts_runtime.discard_until_start = true;
    xiaozhi_tts_drop_queued_frames();
    const uint8_t reset_feeder = XIAOZHI_TTS_CMD_RESET_FEEDER;
    if (xQueueOverwrite(s_tts_runtime.control_slots, &reset_feeder) != pdPASS) {
        ESP_LOGW(TAG, "Failed to queue serialized TTS feeder reset");
    }
}

static esp_mcp_value_t camera_take_photo_callback(const esp_mcp_property_list_t *properties);

/* Thin MCP entry point: runs in the small websocket_task stack. Offloads the
 * stack-heavy capture + TLS-explain work to cam_worker and blocks for it. */
static esp_mcp_value_t camera_take_photo_callback(const esp_mcp_property_list_t *properties)
{
    if (s_cam_worker == NULL || s_cam_done_sem == NULL || s_cam_call_mutex == NULL) {
        ESP_LOGE(TAG, "camera worker not ready");
        return esp_mcp_value_create_bool(false);
    }
    if (xSemaphoreTake(s_cam_call_mutex, pdMS_TO_TICKS(30000)) != pdTRUE) {
        ESP_LOGE(TAG, "camera worker busy (timeout)");
        return esp_mcp_value_create_bool(false);
    }
    s_cam_job.properties = properties;
    s_cam_job.result = esp_mcp_value_create_bool(false);
    xTaskNotifyGive(s_cam_worker);
    if (xSemaphoreTake(s_cam_done_sem, pdMS_TO_TICKS(60000)) != pdTRUE) {
        ESP_LOGE(TAG, "camera worker timeout");
        xSemaphoreGive(s_cam_call_mutex);
        return esp_mcp_value_create_bool(false);
    }
    esp_mcp_value_t r = s_cam_job.result;
    xSemaphoreGive(s_cam_call_mutex);
    return r;
}

static esp_mcp_value_t camera_take_photo_do(const esp_mcp_property_list_t *properties)
{
    /* The MCP engine rejects any call that omits a declared tool property, and
     * the xiaozhi server / external MCP clients normally invoke this tool with
     * empty arguments ("application-managed camera"). So question/url/token are
     * no longer declared as required properties; we supply them here instead:
     * a default question, the default vision endpoint, and the device's own
     * auth token from the NVS keystore. Callers that DO pass them still win via
     * the same fallback logic below. */
    const char *question = esp_mcp_property_list_get_property_string(properties, "question");
    const char *url = esp_mcp_property_list_get_property_string(properties, "url");
    const char *token = esp_mcp_property_list_get_property_string(properties, "token");

    const char *kDefaultQuestion = app_language_text(
        "请描述你看到的画面内容。",
        "Please describe what you see in the image.");
    static const char *kDefaultUrl = "https://api.xiaozhi.me/vision/explain";

    if (question == NULL || question[0] == '\0') {
        question = kDefaultQuestion;
    }
    if (url == NULL || url[0] == '\0') {
        url = kDefaultUrl;
    }
    char dev_token[160];
    if (token == NULL || token[0] == '\0') {
        /* fall back to the device's own xiaozhi auth token (same source the
         * websocket uses to authenticate with the server). It lives in the NVS
         * keystore namespace "websocket", key "token". */
        esp_xiaozhi_chat_keystore_t ks;
        if (esp_xiaozhi_chat_keystore_init(&ks, "websocket", false) == ESP_OK) {
            esp_xiaozhi_chat_keystore_get_string(&ks, "token", "", dev_token, sizeof(dev_token));
            esp_xiaozhi_chat_keystore_deinit(&ks);
        }
        if (dev_token[0] != '\0') {
            token = dev_token;
        }
    }

    esp_err_t ret = init_camera_explain_if_needed();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create camera explain client: %s", esp_err_to_name(ret));
        return esp_mcp_value_create_bool(false);
    }

    ret = esp_xiaozhi_camera_set_explain_url(s_camera_explain, url, token);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure explain url: %s", esp_err_to_name(ret));
        return esp_mcp_value_create_bool(false);
    }
#if CONFIG_TBAO_CAMERA_SUPPORT
    /* Capture a JPEG still from the T-Bao S31 PARLIO camera. */
    app_activity_camera_acquire(APP_ACT_CAMERA_MCP);
    int camera_wait_ms = 0;
    while (!app_camera_is_running() && camera_wait_ms < 2000) {
        vTaskDelay(pdMS_TO_TICKS(20));
        camera_wait_ms += 20;
    }
    if (!app_camera_is_running()) {
        app_activity_camera_release(APP_ACT_CAMERA_MCP);
        ESP_LOGE(TAG, "Camera did not start before photo capture timeout");
        return esp_mcp_value_create_bool(false);
    }
    uint8_t *jpeg = NULL;
    size_t jlen = 0;
    esp_err_t cap = app_camera_capture_jpeg(&jpeg, &jlen);
    app_activity_camera_release(APP_ACT_CAMERA_MCP);
    if (cap != ESP_OK || jpeg == NULL) {
        ESP_LOGE(TAG, "Failed to capture photo");
        return esp_mcp_value_create_bool(false);
    }

    esp_xiaozhi_camera_frame_t frame = {
        .data = jpeg,
        .len = jlen,
    };

    char *response = heap_caps_calloc(1, 4096, MALLOC_CAP_8BIT);
    if (response == NULL) {
        app_camera_release_jpeg(jpeg);
        ESP_LOGE(TAG, "Failed to allocate explain response buffer");
        return esp_mcp_value_create_bool(false);
    }

    size_t response_len = 0;
    cap = esp_xiaozhi_camera_explain(s_camera_explain, &frame, question, response, 4096, &response_len);
    app_camera_release_jpeg(jpeg);

    if (cap != ESP_OK) {
        ESP_LOGE(TAG, "Failed to explain photo: %s", esp_err_to_name(cap));
        free(response);
        return esp_mcp_value_create_bool(false);
    }

    esp_mcp_value_t result = esp_mcp_value_create_string(response);
    free(response);
    if (result.type == ESP_MCP_VALUE_TYPE_INVALID) {
        ESP_LOGE(TAG, "Failed to create explain result value");
        return esp_mcp_value_create_bool(false);
    }

    return result;
#else
    return esp_mcp_value_create_bool(false);
#endif
}

static esp_mcp_value_t get_device_status_callback(const esp_mcp_property_list_t* properties)
{
    ESP_LOGI(TAG, "get_device_status_callback called");

    cJSON *status = cJSON_CreateObject();
    if (!status) {
        ESP_LOGE(TAG, "Failed to create status object");
        return esp_mcp_value_create_bool(false);
    }

    cJSON *audio = cJSON_CreateObject();
    if (!audio) {
        ESP_LOGE(TAG, "Failed to create audio object");
        cJSON_Delete(status);
        return esp_mcp_value_create_bool(false);
    }

    cJSON *screen = cJSON_CreateObject();
    if (!screen) {
        ESP_LOGE(TAG, "Failed to create screen object");
        cJSON_Delete(audio);
        cJSON_Delete(status);
        return esp_mcp_value_create_bool(false);
    }

    cJSON *volume_item = cJSON_CreateNumber(current_volume);
    if (!volume_item) {
        ESP_LOGE(TAG, "Failed to create volume item");
        cJSON_Delete(screen);
        cJSON_Delete(audio);
        cJSON_Delete(status);
        return esp_mcp_value_create_bool(false);
    }
    cJSON_AddItemToObject(audio, "volume", volume_item);
    cJSON_AddItemToObject(status, "audio", audio);

    cJSON *brightness_item = cJSON_CreateNumber(current_brightness);
    if (!brightness_item) {
        ESP_LOGE(TAG, "Failed to create brightness item");
        cJSON_Delete(screen);
        cJSON_Delete(status);
        return esp_mcp_value_create_bool(false);
    }
    cJSON_AddItemToObject(screen, "brightness", brightness_item);

    cJSON *theme_item = cJSON_CreateString(current_theme);
    if (!theme_item) {
        ESP_LOGE(TAG, "Failed to create theme item");
        cJSON_Delete(screen);
        cJSON_Delete(status);
        return esp_mcp_value_create_bool(false);
    }
    cJSON_AddItemToObject(screen, "theme", theme_item);
    cJSON_AddItemToObject(status, "screen", screen);

    cJSON *battery = cJSON_CreateObject();
    if (!battery) {
        ESP_LOGE(TAG, "Failed to create battery object");
        cJSON_Delete(status);
        return esp_mcp_value_create_bool(false);
    }
    battery_monitor_state_t battery_state = {0};
    if (battery_monitor_get_state(&battery_state) != ESP_OK) {
        battery_state.battery_percent = -1;
        battery_state.phase = BATTERY_MONITOR_PHASE_UNKNOWN;
    }
    cJSON_AddBoolToObject(battery, "valid", battery_state.valid);
    cJSON_AddNumberToObject(battery, "percent", battery_state.battery_percent);
    cJSON_AddNumberToObject(battery, "voltage_mv", battery_state.vbat_mv);
    cJSON_AddNumberToObject(battery, "vbus_mv", battery_state.vbus_mv);
    cJSON_AddBoolToObject(battery, "charging", battery_state.charging);
    cJSON_AddBoolToObject(battery, "charge_done", battery_state.charge_done);
    cJSON_AddBoolToObject(battery, "power_good", battery_state.power_good);
    cJSON_AddBoolToObject(battery, "power_good_pin", battery_state.power_good_pin);
    cJSON_AddStringToObject(battery, "phase",
                            battery_monitor_phase_name(battery_state.phase));
    cJSON_AddNumberToObject(battery, "faults", battery_state.fault_bits);
    cJSON_AddStringToObject(battery, "detail", battery_state.detail);
    cJSON_AddItemToObject(status, "battery", battery);

    char *json_string = cJSON_Print(status);
    cJSON_Delete(status);

    if (!json_string) {
        ESP_LOGE(TAG, "Failed to print JSON string");
        return esp_mcp_value_create_bool(false);
    }

    esp_mcp_value_t result = esp_mcp_value_create_string(json_string);
    free(json_string);

    if (result.type == ESP_MCP_VALUE_TYPE_INVALID) {
        ESP_LOGE(TAG, "Failed to create string value for device status");
        return esp_mcp_value_create_bool(false);
    }

    return result;
}

/* Public volume / brightness controls (also used by the control center).
 * These can be called from the LVGL task (control-center slider), the MCP
 * task (set_volume / set_brightness tools) and the app task, so guard the
 * hardware access with a mutex to avoid concurrent LEDC / codec access
 * (which caused a misaligned-load crash when the slider was dragged while a
 * MCP brightness call was in flight). */
static SemaphoreHandle_t s_audio_ctrl_mutex = NULL;

/* Created once at init (single-threaded) to avoid a race between multiple
 * tasks lazily creating it. */
void esp_xiaozhi_chat_app_audio_ctrl_lock_init(void)
{
    if (s_audio_ctrl_mutex == NULL) {
        s_audio_ctrl_mutex = xSemaphoreCreateMutex();
    }
}

static esp_err_t set_volume_internal(int volume, bool persist)
{
    if (volume < 0 || volume > 100) {
        ESP_LOGE(TAG, "Invalid volume value: %d", volume);
        return ESP_ERR_INVALID_ARG;
    }
    if (s_audio_ctrl_mutex && xSemaphoreTake(s_audio_ctrl_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }
    current_volume = volume;
    ESP_LOGI(TAG, "Volume set to: %d", current_volume);
    /* Volume 0 must actually be SILENT. The ES8389's volume register maps 0 to a
     * very low (not mute) gain, so a slider dragged to zero would still produce
     * audible output ("拖动滑动条清零没效果"). Explicitly mute the DAC at 0 and
     * unmute for anything above it. */
    if (s_play_dev != NULL) {
        if (volume == 0) {
            esp_codec_dev_set_out_mute(s_play_dev, true);
        } else {
            esp_codec_dev_set_out_mute(s_play_dev, false);
            if (esp_codec_dev_set_out_vol(s_play_dev, volume) != ESP_CODEC_DEV_OK) {
                ESP_LOGW(TAG, "Failed to apply output volume to codec device");
                if (s_audio_ctrl_mutex) {
                    xSemaphoreGive(s_audio_ctrl_mutex);
                }
                return ESP_FAIL;
            }
        }
    }
    s_settings.volume = current_volume;
    if (persist && app_settings_set_volume(current_volume) != ESP_OK) {
        ESP_LOGW(TAG, "Volume changed but could not be persisted");
    }
    if (s_audio_ctrl_mutex) {
        xSemaphoreGive(s_audio_ctrl_mutex);
    }
    return ESP_OK;
}

esp_err_t esp_xiaozhi_chat_app_set_volume(int volume)
{
    return set_volume_internal(volume, true);
}

esp_err_t esp_xiaozhi_chat_app_preview_volume(int volume)
{
    return set_volume_internal(volume, false);
}

esp_err_t esp_xiaozhi_chat_app_commit_volume(void)
{
    if (s_audio_ctrl_mutex && xSemaphoreTake(s_audio_ctrl_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }
    const int volume = current_volume;
    esp_err_t ret = app_settings_set_volume(volume);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Volume changed but could not be persisted");
    }
    if (s_audio_ctrl_mutex) {
        xSemaphoreGive(s_audio_ctrl_mutex);
    }
    return ret;
}

/* Minimum backlight to keep the screen visible. A slider dragged to 0 would
 * otherwise set LEDC duty to 0 -> screen goes fully black and the user cannot
 * see the slider to drag it back ("拉不回来"). A low floor (5%) keeps the
 * panel dim but usable. The slider callback snaps the knob to this value so
 * dragging toward 0 visibly dims the screen instead of doing nothing. */
#define BRIGHTNESS_MIN_PERCENT 5

static esp_err_t set_brightness_internal(int brightness, bool persist)
{
    if (brightness < 0 || brightness > 100) {
        ESP_LOGE(TAG, "Invalid brightness value: %d", brightness);
        return ESP_ERR_INVALID_ARG;
    }
    /* Clamp to a visible floor so the control center can always be recovered. */
    if (brightness < BRIGHTNESS_MIN_PERCENT) {
        brightness = BRIGHTNESS_MIN_PERCENT;
    }
    if (s_audio_ctrl_mutex && xSemaphoreTake(s_audio_ctrl_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }
    current_brightness = brightness;
    ESP_LOGI(TAG, "Brightness set to: %d", current_brightness);
    esp_xiaozhi_chat_display_set_brightness(current_brightness);
    s_settings.brightness = current_brightness;
    if (persist && app_settings_set_brightness(current_brightness) != ESP_OK) {
        ESP_LOGW(TAG, "Brightness changed but could not be persisted");
    }
    if (s_audio_ctrl_mutex) {
        xSemaphoreGive(s_audio_ctrl_mutex);
    }
    return ESP_OK;
}

esp_err_t esp_xiaozhi_chat_app_set_brightness(int brightness)
{
    return set_brightness_internal(brightness, true);
}

esp_err_t esp_xiaozhi_chat_app_preview_brightness(int brightness)
{
    return set_brightness_internal(brightness, false);
}

esp_err_t esp_xiaozhi_chat_app_commit_brightness(void)
{
    if (s_audio_ctrl_mutex && xSemaphoreTake(s_audio_ctrl_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }
    const int brightness = current_brightness;
    esp_err_t ret = app_settings_set_brightness(brightness);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Brightness changed but could not be persisted");
    }
    if (s_audio_ctrl_mutex) {
        xSemaphoreGive(s_audio_ctrl_mutex);
    }
    return ret;
}

int esp_xiaozhi_chat_app_get_volume(void)     { return current_volume; }
int esp_xiaozhi_chat_app_get_brightness(void) { return current_brightness; }

static esp_mcp_value_t set_volume_callback(const esp_mcp_property_list_t* properties)
{
    int volume = esp_mcp_property_list_get_property_int(properties, "volume");
    bool ok = (esp_xiaozhi_chat_app_set_volume(volume) == ESP_OK);
    return esp_mcp_value_create_bool(ok);
}

static esp_mcp_value_t set_brightness_callback(const esp_mcp_property_list_t* properties)
{
    int brightness = esp_mcp_property_list_get_property_int(properties, "brightness");
    bool ok = (esp_xiaozhi_chat_app_set_brightness(brightness) == ESP_OK);
    return esp_mcp_value_create_bool(ok);
}

/* --- music MCP tools ------------------------------------------------------ */

static esp_mcp_value_t music_play_callback(const esp_mcp_property_list_t* properties)
{
    const char *url = esp_mcp_property_list_get_property_string(properties, "url");
    if (url == NULL || *url == '\0') {
        return esp_mcp_value_create_bool(false);
    }
    bool ok = (app_music_player_play(url) == ESP_OK);
    return esp_mcp_value_create_bool(ok);
}

static esp_mcp_value_t music_pause_callback(const esp_mcp_property_list_t* properties)
{
    (void)properties;
    bool ok = (app_music_player_pause() == ESP_OK);
    return esp_mcp_value_create_bool(ok);
}

static esp_mcp_value_t music_resume_callback(const esp_mcp_property_list_t* properties)
{
    (void)properties;
    bool ok = (app_music_player_resume() == ESP_OK);
    return esp_mcp_value_create_bool(ok);
}

static esp_mcp_value_t music_stop_callback(const esp_mcp_property_list_t* properties)
{
    (void)properties;
    bool ok = (app_music_player_stop() == ESP_OK);
    return esp_mcp_value_create_bool(ok);
}

static esp_mcp_value_t music_status_callback(const esp_mcp_property_list_t* properties)
{
    (void)properties;
    return esp_mcp_value_create_string(app_music_player_state_str());
}

static esp_mcp_value_t set_theme_callback(const esp_mcp_property_list_t* properties)
{
    const char *theme = esp_mcp_property_list_get_property_string(properties, "theme");
    if (!theme) {
        ESP_LOGE(TAG, "Failed to get theme");
        return esp_mcp_value_create_bool(false);
    }

    ESP_LOGI(TAG, "Theme set to: %s", theme);
    strncpy(current_theme, theme, sizeof(current_theme) - 1);
    current_theme[sizeof(current_theme) - 1] = '\0';

    esp_xiaozhi_chat_display_set_theme(theme);

    return esp_mcp_value_create_bool(true);
}

static esp_mcp_value_t set_hsv_callback(const esp_mcp_property_list_t* properties)
{
    const char *hsv = esp_mcp_property_list_get_property_array(properties, "HSV");
    if (!hsv) {
        ESP_LOGE(TAG, "Failed to get HSV");
        return esp_mcp_value_create_bool(false);
    }

    ESP_LOGI(TAG, "HSV set to: %s", hsv);
    cJSON *hsv_json = cJSON_Parse(hsv);
    if (!hsv_json) {
        ESP_LOGE(TAG, "Invalid HSV value: %s", hsv);
        return esp_mcp_value_create_bool(false);
    }

    if (!cJSON_IsArray(hsv_json)) {
        ESP_LOGE(TAG, "Invalid HSV value (expect JSON array): %s", hsv);
        cJSON_Delete(hsv_json);
        return esp_mcp_value_create_bool(false);
    }

    int size = cJSON_GetArraySize(hsv_json);
    if (size <= 0 || size > 3) {
        ESP_LOGE(TAG, "Invalid HSV array size=%d (expect 1..3): %s", size, hsv);
        cJSON_Delete(hsv_json);
        return esp_mcp_value_create_bool(false);
    }

    int hue = current_hue;
    int saturation = current_saturation;
    int value = current_value;

    cJSON *h_item = cJSON_GetArrayItem(hsv_json, 0);
    if (h_item && !cJSON_IsNull(h_item)) {
        if (!cJSON_IsNumber(h_item)) {
            ESP_LOGE(TAG, "Invalid HSV[0] type (expect number or null): %s", hsv);
            cJSON_Delete(hsv_json);
            return esp_mcp_value_create_bool(false);
        }
        hue = h_item->valueint;
    }

    cJSON *s_item = (size >= 2) ? cJSON_GetArrayItem(hsv_json, 1) : NULL;
    if (s_item && !cJSON_IsNull(s_item)) {
        if (!cJSON_IsNumber(s_item)) {
            ESP_LOGE(TAG, "Invalid HSV[1] type (expect number or null): %s", hsv);
            cJSON_Delete(hsv_json);
            return esp_mcp_value_create_bool(false);
        }
        saturation = s_item->valueint;
    }

    cJSON *v_item = (size >= 3) ? cJSON_GetArrayItem(hsv_json, 2) : NULL;
    if (v_item && !cJSON_IsNull(v_item)) {
        if (!cJSON_IsNumber(v_item)) {
            ESP_LOGE(TAG, "Invalid HSV[2] type (expect number or null): %s", hsv);
            cJSON_Delete(hsv_json);
            return esp_mcp_value_create_bool(false);
        }
        value = v_item->valueint;
    }
    cJSON_Delete(hsv_json);

    if (hue < 0 || hue > 360 || saturation < 0 || saturation > 100 || value < 0 || value > 100) {
        ESP_LOGE(TAG, "HSV out of range: hue=%d saturation=%d value=%d (expect hue 0-360, sat/val 0-100)", hue, saturation, value);
        return esp_mcp_value_create_bool(false);
    }

    current_hue = hue;
    current_saturation = saturation;
    current_value = value;

    ESP_LOGI(TAG, "HSV set to: hue: %d, saturation: %d, value: %d", hue, saturation, value);
    return esp_mcp_value_create_bool(true);
}

static esp_mcp_value_t set_rgb_callback(const esp_mcp_property_list_t* properties)
{
    const char *rgb = esp_mcp_property_list_get_property_object(properties, "RGB");
    if (!rgb) {
        ESP_LOGE(TAG, "Failed to get RGB");
        return esp_mcp_value_create_bool(false);
    }

    ESP_LOGI(TAG, "RGB set to: %s", rgb);
    cJSON *rgb_json = cJSON_Parse(rgb);
    if (!rgb_json) {
        ESP_LOGE(TAG, "Invalid RGB value: %s", rgb);
        return esp_mcp_value_create_bool(false);
    }

    if (!cJSON_IsObject(rgb_json)) {
        ESP_LOGE(TAG, "Invalid RGB value (expect JSON object): %s", rgb);
        cJSON_Delete(rgb_json);
        return esp_mcp_value_create_bool(false);
    }

    int red = current_red;
    int green = current_green;
    int blue = current_blue;
    bool updated = false;

    cJSON *r_item = cJSON_GetObjectItem(rgb_json, "red");
    if (r_item && !cJSON_IsNull(r_item)) {
        if (!cJSON_IsNumber(r_item)) {
            ESP_LOGE(TAG, "Invalid RGB.red type (expect number or null): %s", rgb);
            cJSON_Delete(rgb_json);
            return esp_mcp_value_create_bool(false);
        }
        red = r_item->valueint;
        updated = true;
    }

    cJSON *g_item = cJSON_GetObjectItem(rgb_json, "green");
    if (g_item && !cJSON_IsNull(g_item)) {
        if (!cJSON_IsNumber(g_item)) {
            ESP_LOGE(TAG, "Invalid RGB.green type (expect number or null): %s", rgb);
            cJSON_Delete(rgb_json);
            return esp_mcp_value_create_bool(false);
        }
        green = g_item->valueint;
        updated = true;
    }

    cJSON *b_item = cJSON_GetObjectItem(rgb_json, "blue");
    if (b_item && !cJSON_IsNull(b_item)) {
        if (!cJSON_IsNumber(b_item)) {
            ESP_LOGE(TAG, "Invalid RGB.blue type (expect number or null): %s", rgb);
            cJSON_Delete(rgb_json);
            return esp_mcp_value_create_bool(false);
        }
        blue = b_item->valueint;
        updated = true;
    }
    cJSON_Delete(rgb_json);

    if (!updated) {
        ESP_LOGE(TAG, "No RGB fields provided (expect at least one of red/green/blue): %s", rgb);
        return esp_mcp_value_create_bool(false);
    }

    if (red < 0 || red > 255 || green < 0 || green > 255 || blue < 0 || blue > 255) {
        ESP_LOGE(TAG, "RGB out of range: red=%d green=%d blue=%d (expect 0-255)", red, green, blue);
        return esp_mcp_value_create_bool(false);
    }

    current_red = red;
    current_green = green;
    current_blue = blue;

    ESP_LOGI(TAG, "RGB set to: red: %d, green: %d, blue: %d", red, green, blue);
    return esp_mcp_value_create_bool(true);
}

static void esp_xiaozhi_chat_app_audio_error(esp_err_t error)
{
    if (error == ESP_OK) {
        return;
    }
    /* Transient failures (mutex timeout, channel already closed, UDP backpressure): log only, no UI error */
    if (error == ESP_ERR_TIMEOUT || error == ESP_FAIL ||
            error == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Send audio transient: %s", esp_err_to_name(error));
        return;
    }
    ESP_LOGE(TAG, "Failed to send audio data: %s", esp_err_to_name(error));
    esp_xiaozhi_chat_display_set_status("Error");
    esp_xiaozhi_chat_display_set_notification("Error", 2000);
    esp_xiaozhi_chat_display_set_emotion("sad");
}

static void esp_xiaozhi_chat_app_audio_event(esp_xiaozhi_chat_event_t event, void *event_data, void *ctx)
{
    esp_xiaozhi_chat_app_t *xiaozhi_chat_app =
        ctx != NULL ? (esp_xiaozhi_chat_app_t *)ctx : s_xiaozhi_chat_app;

    switch (event) {
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_SPEECH_STARTED:
        ESP_LOGI(TAG, "chat start");
        esp_xiaozhi_chat_display_set_status("Speaking...");
        esp_xiaozhi_chat_display_set_emotion("confused");
        app_audio_bargein_set_playing(true);
        break;
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_SPEECH_STOPPED:
        ESP_LOGI(TAG, "chat stop");
        esp_xiaozhi_chat_display_set_status("Ready");
        esp_xiaozhi_chat_display_set_emotion("neutral");
        app_audio_bargein_set_playing(false);
        /* Rearm is owned by the TTS_STATE_STOP handler below; both events
         * fire for the same server message and rearming twice sent a
         * duplicate abort+listen pair. */
        break;
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_TTS_STATE: {
        esp_xiaozhi_chat_tts_state_t *tts = (esp_xiaozhi_chat_tts_state_t *)event_data;
        if (tts) {
            if (tts->state == ESP_XIAOZHI_CHAT_TTS_STATE_START) {
                /* A new server answer opens a fresh playback generation. Stop
                 * feeding the recorder pipeline while the speaker is active:
                 * keeping it running made the AFE(FEED) ringbuffer fill and
                 * the recorder task (prio 10) starve the fetch task, which
                 * ended in a task-watchdog livelock. WakeNet/VAD still run, so
                 * wake-word barge-in keeps working. */
                xiaozhi_afe_set_voice_output(false);
                s_tts_runtime.discard_until_start = false;
                xiaozhi_chat_app_set_tts_playing(xiaozhi_chat_app, true);
                esp_xiaozhi_chat_display_set_status("Speaking...");
                esp_xiaozhi_chat_display_set_emotion("confused");
                motion_behavior_on_emotion("confused");
                app_audio_bargein_set_playing(true);
            } else if (tts->state == ESP_XIAOZHI_CHAT_TTS_STATE_STOP) {
                esp_xiaozhi_chat_display_set_status("Ready");
                esp_xiaozhi_chat_display_set_emotion("neutral");
                app_audio_bargein_set_playing(false);
                (void)xiaozhi_chat_app_force_listen_rearm(xiaozhi_chat_app);
            }
            /* SENTENCE_START: CHAT_TEXT is also emitted, UI updated there */
        }
        break;
    }
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_SYSTEM_CMD:
        if (event_data && strcmp((const char *)event_data, "reboot") == 0) {
            ESP_LOGI(TAG, "System command reboot, restarting");
            esp_restart();
        }
        break;
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_TEXT: {
        esp_xiaozhi_chat_text_data_t *text_data = (esp_xiaozhi_chat_text_data_t *)event_data;
        if (text_data && text_data->text) {
            const char *role_str = (text_data->role == ESP_XIAOZHI_CHAT_TEXT_ROLE_USER) ? "user" : "assistant";
            chat_role_t role = (text_data->role == ESP_XIAOZHI_CHAT_TEXT_ROLE_USER)
                                 ? CHAT_ROLE_USER : CHAT_ROLE_ASSISTANT;
            /* Persist locally and forward to the active page (the face page
             * shows it as an emote toast; the dialogue page appends a bubble). */
            app_chat_history_append(role, text_data->text);
            ui_page_manager_notify_chat(role, text_data->text);
            /* AI-chat music control: "播放/搜/找 XXX" spoken to 小智 starts
             * playback on the 喵伴 (SD match, else online search). */
            if (role == CHAT_ROLE_USER) {
                motion_behavior_on_user_text(text_data->text);
                char song[64];
                if (app_music_intent(text_data->text, song, sizeof(song))) {
                    app_music_play_by_name(song);
                }
            }
            (void)role_str;
        }
        break;
    }
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_EMOJI:
        if (event_data != NULL) {
            esp_xiaozhi_chat_display_set_emotion((const char *)event_data);
            motion_behavior_on_emotion((const char *)event_data);
        }
        break;
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_ERROR: {
        esp_xiaozhi_chat_error_info_t *info = (esp_xiaozhi_chat_error_info_t *)event_data;
        if (info) {
            ESP_LOGE(TAG, "chat error: %s (source: %s)", esp_err_to_name(info->code), info->source ? info->source : "");
        } else {
            ESP_LOGE(TAG, "chat error: unknown");
        }
        esp_xiaozhi_chat_display_set_status("Error");
        esp_xiaozhi_chat_display_set_notification("Error", 2000);
        esp_xiaozhi_chat_display_set_emotion("sad");
        break;
    }
    default:
        break;
    }
}

static void esp_xiaozhi_chat_app_audio_data(const uint8_t *data, int len, void *ctx)
{
    (void)ctx;
    if (s_tts_runtime.discard_until_start) {
        return;
    }
    if (data == NULL || len <= 0 || len > (int)XIAOZHI_TTS_FRAME_MAX_BYTES ||
            s_tts_runtime.ready_slots == NULL || s_tts_runtime.free_slots == NULL) {
        ESP_LOGW(TAG, "Dropping invalid TTS frame (len=%d, queue=%s)", len,
                 s_tts_runtime.ready_slots != NULL ? "ready" : "offline");
        return;
    }

    uint8_t slot = XIAOZHI_TTS_STOP_SLOT;
    if (xQueueReceive(s_tts_runtime.free_slots, &slot, 0) != pdTRUE) {
        static uint32_t s_tts_drop_count = 0;
        ++s_tts_drop_count;
        if (s_tts_drop_count == 1 || (s_tts_drop_count % 50U) == 0U) {
            ESP_LOGW(TAG, "TTS feed queue full, dropping frame (count=%u)",
                     (unsigned)s_tts_drop_count);
        }
        return;
    }

    s_tts_runtime.lengths[slot] = (uint16_t)len;
    memcpy(s_tts_runtime.storage + ((size_t)slot * XIAOZHI_TTS_FRAME_MAX_BYTES),
           data, (size_t)len);
    if (xQueueSend(s_tts_runtime.ready_slots, &slot, 0) != pdTRUE) {
        xQueueSend(s_tts_runtime.free_slots, &slot, 0);
    }
}

static void esp_xiaozhi_chat_app_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    esp_xiaozhi_chat_app_t *xiaozhi_chat_app = (esp_xiaozhi_chat_app_t *)arg;
    (void)event_base;
    switch (event_id) {
    case ESP_XIAOZHI_CHAT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "connected");
        xiaozhi_chat_app_set_transport_connected(xiaozhi_chat_app, true);
        esp_xiaozhi_chat_display_clear_status();
        esp_xiaozhi_chat_display_set_emotion("happy");
        motion_behavior_on_emotion("happy");
        break;
    case ESP_XIAOZHI_CHAT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "disconnected");
        const bool had_active_audio =
            xiaozhi_chat_app_is_wakeuped(xiaozhi_chat_app);
        xiaozhi_chat_app_set_transport_connected(xiaozhi_chat_app, false);
        xiaozhi_chat_app_set_wakeuped(xiaozhi_chat_app, false);
        xiaozhi_chat_app_set_audio_send_blocked(xiaozhi_chat_app, true);
        xiaozhi_chat_app->audio_send_errors = 0;
        xiaozhi_audio_send_clear();
        if (had_active_audio && xiaozhi_chat_app->data_evt_group != NULL) {
            /* A transport event can arrive while the recorder is idle (for
             * example during TTS), so do not rely on the next mic frame to
             * trigger stale-session cleanup. */
            xEventGroupSetBits(xiaozhi_chat_app->data_evt_group,
                               ESP_XIAOZHI_CHAT_APP_OFFLINE);
        }
        esp_xiaozhi_chat_display_clear_status();
        esp_xiaozhi_chat_display_set_emotion("sad");
        break;
    case ESP_XIAOZHI_CHAT_EVENT_AUDIO_CHANNEL_OPENED:
        ESP_LOGI(TAG, "audio channel opened");
        xiaozhi_chat_app->audio_send_errors = 0;
        esp_xiaozhi_chat_display_set_status("Listening...");
        /* Listening is a UI state only. Do not inject a motion-driving
         * emotion here: waking XiaoZhi must preserve the current head angle. */
        esp_xiaozhi_chat_display_set_emotion("neutral");
        break;
    case ESP_XIAOZHI_CHAT_EVENT_AUDIO_CHANNEL_CLOSED:
        ESP_LOGI(TAG, "audio channel closed");
        xiaozhi_chat_app_set_wakeuped(xiaozhi_chat_app, false);
        xiaozhi_chat_app->audio_send_errors = 0;
        esp_xiaozhi_chat_display_set_status("Ready");
        esp_xiaozhi_chat_display_set_emotion("neutral");
        break;
    case ESP_XIAOZHI_CHAT_EVENT_SERVER_GOODBYE:
        ESP_LOGI(TAG, "server goodbye");
        esp_xiaozhi_chat_display_set_status("Goodbye");
        esp_xiaozhi_chat_display_set_notification("Goodbye", 2000);
        esp_xiaozhi_chat_display_set_emotion("neutral");
        xiaozhi_chat_app_set_wakeuped(xiaozhi_chat_app, false);
        if (xiaozhi_chat_app->data_evt_group) {
            xEventGroupSetBits(xiaozhi_chat_app->data_evt_group, ESP_XIAOZHI_CHAT_APP_OFFLINE);
        }
        break;
    default:
        break;
    }
}

static esp_err_t esp_xiaozhi_chat_app_init(esp_xiaozhi_chat_app_t *xiaozhi_chat_app)
{
    ESP_RETURN_ON_FALSE(xiaozhi_chat_app != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid app");

    esp_err_t ret = ESP_OK;
    esp_xiaozhi_chat_info_t info = {0};
    esp_mcp_t *mcp = NULL;
    esp_mcp_property_t *property = NULL;
    esp_mcp_tool_t *tool = NULL;
    xiaozhi_chat_app->chat = 0;

    do {
        ret = esp_xiaozhi_chat_get_info(&info);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to get chat info: %s", esp_err_to_name(ret));
            break;
        }

        ret = esp_mcp_create(&mcp);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create MCP engine: %s", esp_err_to_name(ret));
            break;
        }

        ret = esp_mcp_set_instructions(
            mcp,
            "This device is a differential-drive desktop robot. Use self.motion.drive for forward, reverse, and steering, self.motion.spin for an in-place turn with a caller-selected duration, and self.motion.spin_360 for exactly one full calibrated revolution. For a one-circle request, you MUST call self.motion.spin_360 and must not simulate it with self.motion.drive or invent a duration. Use self.motion.stop to stop, self.motion.get_status to inspect state, and self.servo.set_angle, self.servo.nod, or self.servo.center for head control.");
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set motion MCP instructions: %s",
                     esp_err_to_name(ret));
            break;
        }

        tool = esp_mcp_tool_create("self.get_device_status", "Get device status including audio, screen, battery, and network information", get_device_status_callback);
        esp_mcp_add_tool(mcp, tool);

        tool = esp_mcp_tool_create("self.audio_speaker.set_volume", "Set audio speaker volume (0-100)", set_volume_callback);
        property = esp_mcp_property_create_with_range("volume", 0, 100);
        esp_mcp_tool_add_property(tool, property);
        esp_mcp_add_tool(mcp, tool);

        tool = esp_mcp_tool_create("self.screen.set_brightness", "Set screen brightness (0-100)", set_brightness_callback);
        property = esp_mcp_property_create_with_range("brightness", 0, 100);
        esp_mcp_tool_add_property(tool, property);
        esp_mcp_add_tool(mcp, tool);

        tool = esp_mcp_tool_create("self.screen.set_theme", "Set screen theme (light/dark)", set_theme_callback);
        property = esp_mcp_property_create_with_string("theme", "light");
        esp_mcp_tool_add_property(tool, property);
        esp_mcp_add_tool(mcp, tool);

        tool = esp_mcp_tool_create("self.screen.set_hsv", "Set screen HSV, first value is hue which range is (0, 360), \
                                    second value is saturation which range is (0, 100), \
                                    third value is value which range is (0, 100)", set_hsv_callback);
        property = esp_mcp_property_create_with_array("HSV", "[120, 80, 50]");
        esp_mcp_tool_add_property(tool, property);
        esp_mcp_add_tool(mcp, tool);

        tool = esp_mcp_tool_create("self.screen.set_rgb", "Set screen RGB, red value range is (0, 255), \
                                    green value range is (0, 255), \
                                    blue value range is (0, 255)", set_rgb_callback);
        property = esp_mcp_property_create_with_object("RGB", "{\"red\": 0, \"green\": 120, \"blue\": 240}");
        esp_mcp_tool_add_property(tool, property);
        esp_mcp_add_tool(mcp, tool);

        tool = esp_mcp_tool_create("self.camera.take_photo",
                                   "Take a photo with an application-managed camera and send it to the vision explain service.",
                                   camera_take_photo_callback);
        /* NOTE: question/url/token are intentionally NOT declared as required
         * properties. The MCP engine rejects any tools/call that omits a declared
         * property, but the xiaozhi server / external MCP clients invoke this
         * tool with empty arguments, so the callback supplies them internally
         * (device token + default vision endpoint + default question). */
        esp_mcp_add_tool(mcp, tool);

        tool = esp_mcp_tool_create("self.music.play",
                                   "Play music / audio from a stream URL (mp3/aac/wav). Pauses the TTS channel if needed.",
                                   music_play_callback);
        property = esp_mcp_property_create_with_string("url", "https://example.com/song.mp3");
        esp_mcp_tool_add_property(tool, property);
        esp_mcp_add_tool(mcp, tool);

        tool = esp_mcp_tool_create("self.music.pause", "Pause the currently playing music.", music_pause_callback);
        esp_mcp_add_tool(mcp, tool);

        tool = esp_mcp_tool_create("self.music.resume", "Resume paused music.", music_resume_callback);
        esp_mcp_add_tool(mcp, tool);

        tool = esp_mcp_tool_create("self.music.stop", "Stop playing music.", music_stop_callback);
        esp_mcp_add_tool(mcp, tool);

        tool = esp_mcp_tool_create("self.music.get_status", "Get current music playback state (playing/paused/stopped/error).", music_status_callback);
        esp_mcp_add_tool(mcp, tool);

        /* The MCP SDK inserts each tool at the head of its list. Register motion
         * tools last so they occupy the first tools/list page when the server
         * uses the default 8192-byte discovery budget. */
        ret = motion_mcp_register_tools(mcp);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register motion MCP tools: %s",
                     esp_err_to_name(ret));
            break;
        }

        esp_xiaozhi_chat_config_t chat_config = {0};
        chat_config.audio_type = ESP_XIAOZHI_CHAT_AUDIO_TYPE_OPUS;
        chat_config.audio_callback = esp_xiaozhi_chat_app_audio_data;
        chat_config.audio_callback_ctx = xiaozhi_chat_app;
        chat_config.event_callback = esp_xiaozhi_chat_app_audio_event;
        chat_config.event_callback_ctx = xiaozhi_chat_app;
        chat_config.mcp_engine = mcp;
        chat_config.owns_mcp_engine = true;
        chat_config.has_mqtt_config = info.has_mqtt_config;
        chat_config.has_websocket_config = info.has_websocket_config;
#if defined(CONFIG_XIAOZHI_CHAT_APP_TRANSPORT_WEBSOCKET)
        chat_config.has_mqtt_config = false;
#endif
        ret = esp_xiaozhi_chat_init(&chat_config, &xiaozhi_chat_app->chat);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to init chat: %s", esp_err_to_name(ret));
            break;
        }
        mcp = NULL;

        ret = esp_xiaozhi_chat_start(xiaozhi_chat_app->chat);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start chat: %s", esp_err_to_name(ret));
            break;
        }
    } while (0);

    if (ret != ESP_OK && xiaozhi_chat_app->chat != 0) {
        esp_err_t cleanup_ret = esp_xiaozhi_chat_deinit(xiaozhi_chat_app->chat);
        if (cleanup_ret == ESP_OK) {
            xiaozhi_chat_app->chat = 0;
        } else {
            ESP_LOGW(TAG, "Failed to clean up chat after init error: %s",
                     esp_err_to_name(cleanup_ret));
        }
    }
    if (mcp != NULL) {
        esp_mcp_destroy(mcp);
    }
    esp_xiaozhi_chat_free_info(&info);

    return ret;
}

static bool xiaozhi_chat_app_queue_wakeup(esp_xiaozhi_chat_app_t *xiaozhi_chat_app)
{
    if (xiaozhi_chat_app == NULL || xiaozhi_chat_app->chat == 0 ||
            xiaozhi_chat_app->state_mutex == NULL) {
        return false;
    }

    bool accepted = false;
    xSemaphoreTake(xiaozhi_chat_app->state_mutex, portMAX_DELAY);
    if (!xiaozhi_chat_app->wakeuped && !xiaozhi_chat_app->wake_pending) {
        xiaozhi_chat_app->wake_pending = true;
        accepted = true;
    }
    xSemaphoreGive(xiaozhi_chat_app->state_mutex);

    /* The AFE can detect a wake word while the boot splash/emote renderer is
     * still taking ownership of the LCD. Keep the request latched, but defer
     * the ONLINE transition until display initialization has completed. */
    bool display_ready = false;
    xSemaphoreTake(xiaozhi_chat_app->state_mutex, portMAX_DELAY);
    display_ready = xiaozhi_chat_app->display_ready;
    xSemaphoreGive(xiaozhi_chat_app->state_mutex);

    if (accepted && display_ready) {
        xEventGroupSetBits(xiaozhi_chat_app->data_evt_group,
                           ESP_XIAOZHI_CHAT_APP_ONLINE);
    }
    return accepted;
}

static void xiaozhi_chat_app_set_display_ready(
    esp_xiaozhi_chat_app_t *xiaozhi_chat_app, bool ready)
{
    if (xiaozhi_chat_app == NULL || xiaozhi_chat_app->state_mutex == NULL) {
        return;
    }

    bool release_pending_wake = false;
    xSemaphoreTake(xiaozhi_chat_app->state_mutex, portMAX_DELAY);
    xiaozhi_chat_app->display_ready = ready;
    release_pending_wake = ready && xiaozhi_chat_app->wake_pending &&
                           !xiaozhi_chat_app->wakeuped;
    xSemaphoreGive(xiaozhi_chat_app->state_mutex);

    if (release_pending_wake && xiaozhi_chat_app->data_evt_group != NULL) {
        ESP_LOGI(TAG, "Display ready; releasing queued Xiaozhi wake");
        xEventGroupSetBits(xiaozhi_chat_app->data_evt_group,
                           ESP_XIAOZHI_CHAT_APP_ONLINE);
    }
}

static bool xiaozhi_chat_app_queue_active_wakeup(
    esp_xiaozhi_chat_app_t *xiaozhi_chat_app)
{
    if (xiaozhi_chat_app == NULL || xiaozhi_chat_app->chat == 0 ||
            xiaozhi_chat_app->state_mutex == NULL) {
        return false;
    }

    bool accepted = false;
    xSemaphoreTake(xiaozhi_chat_app->state_mutex, portMAX_DELAY);
    if (xiaozhi_chat_app->wakeuped &&
            xiaozhi_chat_app->transport_connected &&
            !xiaozhi_chat_app->wake_interrupt_pending) {
        /* Keep the request pending until the abort + listen/start sequence
         * succeeds. If the current socket is stale, the normal disconnect
         * path will reconnect this pending request on a fresh session. */
        xiaozhi_chat_app->wake_interrupt_pending = true;
        xiaozhi_chat_app->wake_pending = true;
        accepted = true;
    }
    xSemaphoreGive(xiaozhi_chat_app->state_mutex);

    if (accepted && xiaozhi_chat_app->data_evt_group != NULL) {
        xEventGroupSetBits(xiaozhi_chat_app->data_evt_group,
                           ESP_XIAOZHI_CHAT_APP_WAKE_INTERRUPT);
    }
    return accepted;
}

static bool xiaozhi_chat_app_is_transport_connected(
    esp_xiaozhi_chat_app_t *xiaozhi_chat_app)
{
    if (xiaozhi_chat_app == NULL || xiaozhi_chat_app->state_mutex == NULL) {
        return false;
    }

    xSemaphoreTake(xiaozhi_chat_app->state_mutex, portMAX_DELAY);
    const bool connected = xiaozhi_chat_app->transport_connected;
    xSemaphoreGive(xiaozhi_chat_app->state_mutex);
    return connected;
}

static void xiaozhi_chat_app_set_audio_send_blocked(
    esp_xiaozhi_chat_app_t *xiaozhi_chat_app, bool blocked)
{
    if (xiaozhi_chat_app == NULL || xiaozhi_chat_app->state_mutex == NULL) {
        return;
    }

    xSemaphoreTake(xiaozhi_chat_app->state_mutex, portMAX_DELAY);
    xiaozhi_chat_app->audio_send_blocked = blocked;
    xSemaphoreGive(xiaozhi_chat_app->state_mutex);
}

static bool xiaozhi_chat_app_is_audio_send_blocked(
    esp_xiaozhi_chat_app_t *xiaozhi_chat_app)
{
    if (xiaozhi_chat_app == NULL || xiaozhi_chat_app->state_mutex == NULL) {
        return true;
    }

    xSemaphoreTake(xiaozhi_chat_app->state_mutex, portMAX_DELAY);
    const bool blocked = xiaozhi_chat_app->audio_send_blocked;
    xSemaphoreGive(xiaozhi_chat_app->state_mutex);
    return blocked;
}

static void xiaozhi_chat_app_set_wake_pending(
    esp_xiaozhi_chat_app_t *xiaozhi_chat_app, bool pending)
{
    if (xiaozhi_chat_app == NULL || xiaozhi_chat_app->state_mutex == NULL) {
        return;
    }

    xSemaphoreTake(xiaozhi_chat_app->state_mutex, portMAX_DELAY);
    xiaozhi_chat_app->wake_pending = pending;
    xSemaphoreGive(xiaozhi_chat_app->state_mutex);
}

static void xiaozhi_chat_app_set_wakeuped(
    esp_xiaozhi_chat_app_t *xiaozhi_chat_app, bool wakeuped)
{
    if (xiaozhi_chat_app == NULL || xiaozhi_chat_app->state_mutex == NULL) {
        return;
    }

    xSemaphoreTake(xiaozhi_chat_app->state_mutex, portMAX_DELAY);
    xiaozhi_chat_app->wakeuped = wakeuped;
    if (!wakeuped) {
        xiaozhi_chat_app->tts_playing = false;
        xiaozhi_chat_app->listen_rearm_pending = false;
        xiaozhi_chat_app->wake_interrupt_pending = false;
    }
    xSemaphoreGive(xiaozhi_chat_app->state_mutex);

    /* Tie the AFE wake/VAD state to the conversation lifetime. Without this
     * the wrapper's wakeup timer expires during a long answer, disables VAD
     * and emits WAKEUP_END; every later turn then lacks local VAD events and
     * no longer behaves like a fresh wake. */
    xiaozhi_afe_set_keep_awake(wakeuped);
}

static void xiaozhi_chat_app_set_tts_playing(
    esp_xiaozhi_chat_app_t *xiaozhi_chat_app, bool playing)
{
    if (xiaozhi_chat_app == NULL || xiaozhi_chat_app->state_mutex == NULL) {
        return;
    }

    xSemaphoreTake(xiaozhi_chat_app->state_mutex, portMAX_DELAY);
    xiaozhi_chat_app->tts_playing = playing;
    xSemaphoreGive(xiaozhi_chat_app->state_mutex);
}

static bool xiaozhi_chat_app_is_tts_playing(
    esp_xiaozhi_chat_app_t *xiaozhi_chat_app)
{
    if (xiaozhi_chat_app == NULL || xiaozhi_chat_app->state_mutex == NULL) {
        return false;
    }

    xSemaphoreTake(xiaozhi_chat_app->state_mutex, portMAX_DELAY);
    const bool playing = xiaozhi_chat_app->tts_playing;
    xSemaphoreGive(xiaozhi_chat_app->state_mutex);
    return playing;
}

static bool xiaozhi_chat_app_is_listen_rearm_pending(
    esp_xiaozhi_chat_app_t *xiaozhi_chat_app)
{
    if (xiaozhi_chat_app == NULL || xiaozhi_chat_app->state_mutex == NULL) {
        return false;
    }

    xSemaphoreTake(xiaozhi_chat_app->state_mutex, portMAX_DELAY);
    const bool pending = xiaozhi_chat_app->listen_rearm_pending;
    xSemaphoreGive(xiaozhi_chat_app->state_mutex);
    return pending;
}

static bool xiaozhi_chat_app_request_listen_rearm(
    esp_xiaozhi_chat_app_t *xiaozhi_chat_app)
{
    if (xiaozhi_chat_app == NULL || xiaozhi_chat_app->state_mutex == NULL) {
        return false;
    }

    bool accepted = false;
    xSemaphoreTake(xiaozhi_chat_app->state_mutex, portMAX_DELAY);
    if (xiaozhi_chat_app->wakeuped &&
            !xiaozhi_chat_app->listen_rearm_pending) {
        xiaozhi_chat_app->tts_playing = false;
        xiaozhi_chat_app->listen_rearm_pending = true;
        accepted = true;
    }
    xSemaphoreGive(xiaozhi_chat_app->state_mutex);

    if (accepted && xiaozhi_chat_app->data_evt_group != NULL) {
        ESP_LOGI(TAG, "listen rearm requested");
        xEventGroupSetBits(xiaozhi_chat_app->data_evt_group,
                           ESP_XIAOZHI_CHAT_APP_LISTEN_REARM);
    }
    return accepted;
}

/* Like request_listen_rearm(), but always queues a listen start even if a
 * barge-in already queued one during the answer. A listen sent while the
 * server was still speaking can be ignored, so the authoritative TTS-stop
 * transition must be able to re-send it unconditionally. */
static bool xiaozhi_chat_app_force_listen_rearm(
    esp_xiaozhi_chat_app_t *xiaozhi_chat_app)
{
    if (xiaozhi_chat_app == NULL || xiaozhi_chat_app->state_mutex == NULL) {
        return false;
    }

    bool accepted = false;
    xSemaphoreTake(xiaozhi_chat_app->state_mutex, portMAX_DELAY);
    if (xiaozhi_chat_app->wakeuped) {
        xiaozhi_chat_app->tts_playing = false;
        xiaozhi_chat_app->listen_rearm_pending = true;
        accepted = true;
    }
    xSemaphoreGive(xiaozhi_chat_app->state_mutex);

    if (accepted && xiaozhi_chat_app->data_evt_group != NULL) {
        ESP_LOGI(TAG, "listen rearm requested (forced after TTS stop)");
        xEventGroupSetBits(xiaozhi_chat_app->data_evt_group,
                           ESP_XIAOZHI_CHAT_APP_LISTEN_REARM);
    }
    return accepted;
}

static bool xiaozhi_chat_app_is_wakeuped(
    esp_xiaozhi_chat_app_t *xiaozhi_chat_app)
{
    if (xiaozhi_chat_app == NULL || xiaozhi_chat_app->state_mutex == NULL) {
        return false;
    }

    xSemaphoreTake(xiaozhi_chat_app->state_mutex, portMAX_DELAY);
    const bool wakeuped = xiaozhi_chat_app->wakeuped;
    xSemaphoreGive(xiaozhi_chat_app->state_mutex);
    return wakeuped;
}

static void xiaozhi_chat_app_set_transport_connected(
    esp_xiaozhi_chat_app_t *xiaozhi_chat_app, bool connected)
{
    if (xiaozhi_chat_app == NULL || xiaozhi_chat_app->state_mutex == NULL) {
        return;
    }

    xSemaphoreTake(xiaozhi_chat_app->state_mutex, portMAX_DELAY);
    xiaozhi_chat_app->transport_connected = connected;
    xiaozhi_chat_app->audio_send_blocked = !connected;
    const bool pending = xiaozhi_chat_app->wake_pending;
    const bool display_ready = xiaozhi_chat_app->display_ready;
    xSemaphoreGive(xiaozhi_chat_app->state_mutex);

    if (xiaozhi_chat_app->data_evt_group == NULL) {
        return;
    }
    if (connected && pending && display_ready) {
        /* A wake detected while the socket was reconnecting is retried as
         * soon as the transport reports connected. */
        xEventGroupSetBits(xiaozhi_chat_app->data_evt_group,
                           ESP_XIAOZHI_CHAT_APP_ONLINE);
    } else if (!connected) {
        xEventGroupSetBits(xiaozhi_chat_app->data_evt_group,
                           ESP_XIAOZHI_CHAT_APP_OFFLINE);
        if (pending && display_ready) {
            xEventGroupSetBits(xiaozhi_chat_app->data_evt_group,
                               ESP_XIAOZHI_CHAT_APP_ONLINE);
        }
    }
}

static esp_err_t xiaozhi_audio_send_init(void)
{
    if (s_audio_send_runtime.storage != NULL) {
        return ESP_OK;
    }

    const size_t storage_size =
        (size_t)XIAOZHI_AUDIO_SEND_QUEUE_DEPTH * ESP_XIAOZHI_CHAT_REC_READ_SIZE;
    s_audio_send_runtime.storage = heap_caps_calloc(
        1, storage_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_audio_send_runtime.storage == NULL) {
        ESP_LOGE(TAG, "Failed to allocate audio send queue (%u bytes)",
                 (unsigned)storage_size);
        return ESP_ERR_NO_MEM;
    }

    s_audio_send_runtime.free_slots =
        xQueueCreate(XIAOZHI_AUDIO_SEND_QUEUE_DEPTH, sizeof(uint8_t));
    s_audio_send_runtime.ready_slots =
        xQueueCreate(XIAOZHI_AUDIO_SEND_QUEUE_DEPTH, sizeof(uint8_t));
    if (s_audio_send_runtime.free_slots == NULL ||
            s_audio_send_runtime.ready_slots == NULL) {
        ESP_LOGE(TAG, "Failed to create audio send slot queues");
        if (s_audio_send_runtime.free_slots != NULL) {
            vQueueDelete(s_audio_send_runtime.free_slots);
        }
        if (s_audio_send_runtime.ready_slots != NULL) {
            vQueueDelete(s_audio_send_runtime.ready_slots);
        }
        heap_caps_free(s_audio_send_runtime.storage);
        memset(&s_audio_send_runtime, 0, sizeof(s_audio_send_runtime));
        return ESP_ERR_NO_MEM;
    }

    for (uint8_t slot = 0; slot < XIAOZHI_AUDIO_SEND_QUEUE_DEPTH; ++slot) {
        xQueueSend(s_audio_send_runtime.free_slots, &slot, 0);
    }

    ESP_LOGI(TAG, "Audio send queue started (depth=%u, frame=%u bytes, PSRAM payloads)",
             (unsigned)XIAOZHI_AUDIO_SEND_QUEUE_DEPTH,
             (unsigned)ESP_XIAOZHI_CHAT_REC_READ_SIZE);
    return ESP_OK;
}

static void xiaozhi_audio_send_clear(void)
{
    if (s_audio_send_runtime.ready_slots == NULL ||
            s_audio_send_runtime.free_slots == NULL) {
        return;
    }

    uint8_t slot;
    while (xQueueReceive(s_audio_send_runtime.ready_slots, &slot, 0) == pdTRUE) {
        xQueueSend(s_audio_send_runtime.free_slots, &slot, 0);
    }
}

static void xiaozhi_audio_send_release(void)
{
    xiaozhi_audio_send_clear();
    if (s_audio_send_runtime.free_slots != NULL) {
        vQueueDelete(s_audio_send_runtime.free_slots);
    }
    if (s_audio_send_runtime.ready_slots != NULL) {
        vQueueDelete(s_audio_send_runtime.ready_slots);
    }
    heap_caps_free(s_audio_send_runtime.storage);
    memset(&s_audio_send_runtime, 0, sizeof(s_audio_send_runtime));
}

static bool xiaozhi_audio_send_enqueue(esp_xiaozhi_chat_app_t *xiaozhi_chat_app,
                                       const uint8_t *data, size_t len)
{
    if (xiaozhi_chat_app == NULL || data == NULL || len == 0 ||
            len > ESP_XIAOZHI_CHAT_REC_READ_SIZE ||
            xiaozhi_chat_app_is_audio_send_blocked(xiaozhi_chat_app) ||
            s_audio_send_runtime.free_slots == NULL ||
            s_audio_send_runtime.ready_slots == NULL ||
            s_audio_send_runtime.storage == NULL) {
        return false;
    }

    uint8_t slot;
    if (xQueueReceive(s_audio_send_runtime.free_slots, &slot, 0) != pdTRUE) {
        /* Realtime audio must not back-pressure the recorder. Reuse the
         * oldest queued frame when the network sender falls behind. */
        if (xQueueReceive(s_audio_send_runtime.ready_slots, &slot, 0) != pdTRUE) {
            return false;
        }
        s_audio_send_runtime.dropped_frames++;
        if ((s_audio_send_runtime.dropped_frames & 0x1FU) == 1U) {
            ESP_LOGW(TAG, "Audio send queue full, dropping stale frame (count=%u)",
                     (unsigned)s_audio_send_runtime.dropped_frames);
        }
    }

    s_audio_send_runtime.lengths[slot] = (uint16_t)len;
    memcpy(s_audio_send_runtime.storage +
               ((size_t)slot * ESP_XIAOZHI_CHAT_REC_READ_SIZE),
           data, len);
    if (xQueueSend(s_audio_send_runtime.ready_slots, &slot, 0) != pdTRUE) {
        xQueueSend(s_audio_send_runtime.free_slots, &slot, 0);
        return false;
    }

    if (xiaozhi_chat_app->data_evt_group != NULL) {
        xEventGroupSetBits(xiaozhi_chat_app->data_evt_group,
                           ESP_XIAOZHI_CHAT_APP_AUDIO_SEND);
    }
    return true;
}

static void xiaozhi_audio_send_drain(esp_xiaozhi_chat_app_t *xiaozhi_chat_app)
{
    if (xiaozhi_chat_app == NULL || s_audio_send_runtime.ready_slots == NULL) {
        return;
    }

    uint8_t slot;
    while (xQueueReceive(s_audio_send_runtime.ready_slots, &slot, 0) == pdTRUE) {
        esp_err_t send_ret = ESP_ERR_INVALID_STATE;
        if (xiaozhi_chat_app_is_wakeuped(xiaozhi_chat_app) &&
                xiaozhi_chat_app_is_transport_connected(xiaozhi_chat_app) &&
                xiaozhi_chat_app->chat != 0) {
            const size_t len = s_audio_send_runtime.lengths[slot];
            send_ret = esp_xiaozhi_chat_send_audio_data(
                xiaozhi_chat_app->chat,
                (const char *)(s_audio_send_runtime.storage +
                               ((size_t)slot * ESP_XIAOZHI_CHAT_REC_READ_SIZE)),
                len);
        }
        xQueueSend(s_audio_send_runtime.free_slots, &slot, 0);

        if (send_ret == ESP_OK) {
            xiaozhi_chat_app->audio_send_errors = 0;
            if (s_log_first_audio_frame) {
                s_log_first_audio_frame = false;
                ESP_LOGI(TAG, "first audio frame after rearm sent (%u bytes)",
                         (unsigned)s_audio_send_runtime.lengths[slot]);
            }
            static uint32_t tx_frames;
            if (++tx_frames % 50U == 0U) {
                ESP_LOGI(TAG, "audio tx frames=%u (upload path alive)",
                         (unsigned)tx_frames);
            }
            continue;
        }

        /* Match the official application loop: once the transport rejects a
         * packet, discard the queued remainder so the producer cannot stall
         * behind an undrainable queue. */
        xiaozhi_audio_send_clear();
        esp_xiaozhi_chat_app_audio_error(send_ret);
        if (send_ret == ESP_ERR_INVALID_STATE || send_ret == ESP_FAIL ||
                send_ret == ESP_ERR_TIMEOUT) {
            xiaozhi_chat_app->audio_send_errors++;
        }
        /* A failed write is a session boundary, including a TX-lock timeout.
         * Stop microphone production before the recorder can refill the queue;
         * the offline path then closes the stale audio channel and the next
         * wake opens a fresh WebSocket session. */
        xiaozhi_chat_app_set_audio_send_blocked(xiaozhi_chat_app, true);
        xiaozhi_chat_app_set_transport_connected(xiaozhi_chat_app, false);
        xiaozhi_chat_app_set_wakeuped(xiaozhi_chat_app, false);
        xiaozhi_chat_app->audio_send_errors = 0;
        if (xiaozhi_chat_app->data_evt_group != NULL) {
            xEventGroupSetBits(xiaozhi_chat_app->data_evt_group,
                               ESP_XIAOZHI_CHAT_APP_OFFLINE);
        }
        break;
    }
}

static void xiaozhi_chat_app_handle_wake_interrupt(
    esp_xiaozhi_chat_app_t *xiaozhi_chat_app)
{
    if (xiaozhi_chat_app == NULL) {
        return;
    }

    if (xiaozhi_chat_app->data_evt_group != NULL) {
        xEventGroupClearBits(xiaozhi_chat_app->data_evt_group,
                             ESP_XIAOZHI_CHAT_APP_WAKE_INTERRUPT |
                                 ESP_XIAOZHI_CHAT_APP_LISTEN_REARM |
                                 ESP_XIAOZHI_CHAT_APP_AUDIO_SEND);
    }

    if (!xiaozhi_chat_app_is_wakeuped(xiaozhi_chat_app) ||
            !xiaozhi_chat_app_is_transport_connected(xiaozhi_chat_app) ||
            xiaozhi_chat_app->chat == 0) {
        xiaozhi_audio_send_clear();
        return;
    }

    /* Do not let frames captured before the wake word leak into the new turn. */
    xiaozhi_audio_send_clear();
    xiaozhi_tts_feed_discard();
    xiaozhi_chat_app_set_tts_playing(xiaozhi_chat_app, false);
    app_audio_bargein_set_playing(false);
    /* A wake word aborts the answer; resume PCM production for the new turn. */
    xiaozhi_afe_set_voice_output(true);

    /* Match the official client: an active wake word aborts the old answer,
     * then immediately starts a fresh automatic listening turn. */
    esp_err_t ret = esp_xiaozhi_chat_send_abort_speaking(
        xiaozhi_chat_app->chat,
        ESP_XIAOZHI_CHAT_ABORT_SPEAKING_REASON_WAKE_WORD_DETECTED);
    if (ret == ESP_OK) {
        ret = esp_xiaozhi_chat_send_start_listening(
            xiaozhi_chat_app->chat, ESP_XIAOZHI_CHAT_LISTENING_MODE_AUTO);
    }
    if (ret != ESP_OK) {
        esp_xiaozhi_chat_app_audio_error(ret);
        /* Keep wake_pending set so the disconnect path opens a new session. */
        xiaozhi_chat_app_set_transport_connected(xiaozhi_chat_app, false);
        xiaozhi_chat_app_set_wakeuped(xiaozhi_chat_app, false);
        xiaozhi_audio_send_clear();
        if (xiaozhi_chat_app->data_evt_group != NULL) {
            xEventGroupSetBits(xiaozhi_chat_app->data_evt_group,
                               ESP_XIAOZHI_CHAT_APP_OFFLINE);
        }
        return;
    }

    xSemaphoreTake(xiaozhi_chat_app->state_mutex, portMAX_DELAY);
    xiaozhi_chat_app->wake_pending = false;
    xiaozhi_chat_app->wake_interrupt_pending = false;
    xiaozhi_chat_app->listen_rearm_pending = false;
    xiaozhi_chat_app->tts_playing = false;
    xSemaphoreGive(xiaozhi_chat_app->state_mutex);
    ESP_LOGI(TAG, "wakeup interrupt: listening restarted");
}

static void esp_xiaozhi_chat_app_audio_recorder(void *event, void *ctx)
{
    esp_gmf_afe_evt_t *afe_evt = (esp_gmf_afe_evt_t *)event;
    esp_xiaozhi_chat_app_t *xiaozhi_chat_app = (esp_xiaozhi_chat_app_t *)ctx;

    switch (afe_evt->type) {
    case ESP_GMF_AFE_EVT_WAKEUP_START:
        if (xiaozhi_chat_app_is_wakeuped(xiaozhi_chat_app)) {
            if (xiaozhi_chat_app_queue_active_wakeup(xiaozhi_chat_app)) {
                ESP_LOGI(TAG, "wakeup start (interrupt queued)");
            } else {
                ESP_LOGD(TAG, "wakeup interrupt already pending");
            }
        } else if (xiaozhi_chat_app_queue_wakeup(xiaozhi_chat_app)) {
            ESP_LOGI(TAG, "wakeup start (queued)");
        } else {
            ESP_LOGD(TAG, "wakeup ignored while conversation is active or opening");
        }
        break;
    case ESP_GMF_AFE_EVT_WAKEUP_END:
        ESP_LOGI(TAG, "wakeup end");
        break;
    case ESP_GMF_AFE_EVT_VAD_START:
        ESP_LOGI(TAG, "vad start (wake=%d conn=%d tts=%d rearm=%d afe_out=%d)",
                 xiaozhi_chat_app != NULL && xiaozhi_chat_app_is_wakeuped(xiaozhi_chat_app) ? 1 : 0,
                 xiaozhi_chat_app != NULL && xiaozhi_chat_app_is_transport_connected(xiaozhi_chat_app) ? 1 : 0,
                 xiaozhi_chat_app != NULL && xiaozhi_chat_app_is_tts_playing(xiaozhi_chat_app) ? 1 : 0,
                 xiaozhi_chat_app != NULL && xiaozhi_chat_app_is_listen_rearm_pending(xiaozhi_chat_app) ? 1 : 0,
                 s_afe_voice_output ? 1 : 0);
        app_audio_bargein_on_vad(true);
        /* The user is speaking and the server is not: the PCM path must be
         * open. This also recovers the gate if a TTS-stop event was lost and
         * left voice output disabled (VAD still fires because it is emitted
         * before the voice-output gate in the AFE result callback). */
        if (xiaozhi_chat_app != NULL &&
                xiaozhi_chat_app_is_wakeuped(xiaozhi_chat_app) &&
                !xiaozhi_chat_app_is_tts_playing(xiaozhi_chat_app)) {
            xiaozhi_afe_set_voice_output(true);
        }
        break;
    case ESP_GMF_AFE_EVT_VAD_END:
        ESP_LOGI(TAG, "vad end");
        app_audio_bargein_on_vad(false);
        /* auto mode is server-VAD on this backend: it ends the turn itself.
         * Sending listen/stop here closed the wake-phrase segment as a full
         * turn (the server burned the first answer on a greeting) and the
         * real question then overlapped that playback with an unmuted mic. */
        break;
    case ESP_GMF_AFE_EVT_VCMD_DECT_TIMEOUT:
        ESP_LOGI(TAG, "vcmd detect timeout");
        break;
    default:
        break;
    }
}

/* Called by the barge-in controller when the user's speech is detected over
 * speaker playback. Queue a feeder reset; only the TTS worker may stop/run the
 * feeder, so it cannot race a write to the feeder FIFO. */
static void esp_xiaozhi_chat_app_bargein_interrupt(void)
{
    ESP_LOGI(TAG, "Barge-in: halting current TTS playback");
    xiaozhi_tts_feed_discard();
    app_audio_bargein_set_playing(false);
    xiaozhi_chat_app_set_tts_playing(s_xiaozhi_chat_app, false);
    /* The user is speaking right now: resume PCM production immediately. */
    xiaozhi_afe_set_voice_output(true);

    /* The user is already speaking while the answer starts. Do NOT go through
     * request_listen_rearm() here: it marks listen_rearm_pending, which drops
     * the microphone frames of the very utterance we are trying to save, and
     * it clears the send queue. Abort the answer and reopen the server-side
     * listen window directly; uploads continue on the next frames. */
    if (s_xiaozhi_chat_app != NULL && s_xiaozhi_chat_app->chat != 0 &&
            xiaozhi_chat_app_is_wakeuped(s_xiaozhi_chat_app) &&
            xiaozhi_chat_app_is_transport_connected(s_xiaozhi_chat_app)) {
        esp_err_t ret = esp_xiaozhi_chat_send_abort_speaking(
            s_xiaozhi_chat_app->chat,
            ESP_XIAOZHI_CHAT_ABORT_SPEAKING_REASON_WAKE_WORD_DETECTED);
        if (ret == ESP_OK) {
            ret = esp_xiaozhi_chat_send_start_listening(
                s_xiaozhi_chat_app->chat,
                ESP_XIAOZHI_CHAT_LISTENING_MODE_AUTO);
        }
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Barge-in listen restart failed: %s",
                     esp_err_to_name(ret));
            (void)xiaozhi_chat_app_request_listen_rearm(s_xiaozhi_chat_app);
        }
    } else {
        (void)xiaozhi_chat_app_request_listen_rearm(s_xiaozhi_chat_app);
    }
}

static void esp_xiaozhi_chat_app_audio_channel(void *pv)
{
    esp_xiaozhi_chat_app_t *xiaozhi_chat_app = (esp_xiaozhi_chat_app_t *)pv;
    uint32_t wake_retry_delay_ms = XIAOZHI_WAKE_RETRY_INITIAL_MS;
    const EventBits_t wait_bits = ESP_XIAOZHI_CHAT_APP_AUDIO_SEND |
                                   ESP_XIAOZHI_CHAT_APP_ONLINE |
                                   ESP_XIAOZHI_CHAT_APP_OFFLINE |
                                   ESP_XIAOZHI_CHAT_APP_LISTEN_REARM |
                                   ESP_XIAOZHI_CHAT_APP_WAKE_INTERRUPT;
    while (1) {
        /* Keep ONLINE as a request bit rather than clearing it on entry. A
         * wake can arrive before the asynchronous WebSocket connection is
         * ready; that request is re-armed by the CONNECTED event below. */
        EventBits_t bits = xEventGroupWaitBits(xiaozhi_chat_app->data_evt_group,
                                               wait_bits, pdFALSE, pdFALSE,
                                               portMAX_DELAY);

        if (bits & ESP_XIAOZHI_CHAT_APP_OFFLINE) {
            xEventGroupClearBits(xiaozhi_chat_app->data_evt_group,
                                 ESP_XIAOZHI_CHAT_APP_OFFLINE |
                                 ESP_XIAOZHI_CHAT_APP_AUDIO_SEND);
            xiaozhi_audio_send_clear();
            if (xiaozhi_chat_app->chat != 0) {
                xiaozhi_chat_app_set_wakeuped(xiaozhi_chat_app, false);
                xiaozhi_chat_app->audio_send_errors = 0;
                esp_xiaozhi_chat_app_audio_error(
                    esp_xiaozhi_chat_close_audio_channel(xiaozhi_chat_app->chat));
            }
        }

        if (bits & ESP_XIAOZHI_CHAT_APP_WAKE_INTERRUPT) {
            xiaozhi_chat_app_handle_wake_interrupt(xiaozhi_chat_app);
            /* A stale LISTEN_REARM/ONLINE snapshot must not issue a second
             * start command after the interrupt has already restarted audio. */
            continue;
        }

        if (bits & ESP_XIAOZHI_CHAT_APP_ONLINE) {
            if (xiaozhi_chat_app->chat == 0) {
                xEventGroupClearBits(xiaozhi_chat_app->data_evt_group,
                                     ESP_XIAOZHI_CHAT_APP_ONLINE);
                ESP_LOGW(TAG, "Ignore online event before chat is initialized");
                continue;
            }
            if (xiaozhi_chat_app_is_wakeuped(xiaozhi_chat_app)) {
                xEventGroupClearBits(xiaozhi_chat_app->data_evt_group,
                                     ESP_XIAOZHI_CHAT_APP_ONLINE);
                xiaozhi_chat_app_set_wake_pending(xiaozhi_chat_app, false);
                continue;
            }

            xEventGroupClearBits(xiaozhi_chat_app->data_evt_group,
                                 ESP_XIAOZHI_CHAT_APP_ONLINE);
            bool audio_opened = false;
            esp_err_t ret = esp_xiaozhi_chat_open_audio_channel(
                xiaozhi_chat_app->chat, &xiaozhi_chat_app->audio, NULL, 0);
            esp_xiaozhi_chat_app_audio_error(ret);
            audio_opened = (ret == ESP_OK);
            if (ret == ESP_OK) {
                /* Upstream sends the buffered wake-word audio before the
                 * detect message (CONFIG_SEND_WAKE_WORD_DATA); the server uses
                 * it for voice-print verification. */
                xiaozhi_preroll_flush(xiaozhi_chat_app);
                ret = esp_xiaozhi_chat_send_wake_word(
                    xiaozhi_chat_app->chat, "你好小智");
                esp_xiaozhi_chat_app_audio_error(ret);
            }
            if (ret == ESP_OK) {
                ret = esp_xiaozhi_chat_send_start_listening(
                    xiaozhi_chat_app->chat,
                    ESP_XIAOZHI_CHAT_LISTENING_MODE_AUTO);
                esp_xiaozhi_chat_app_audio_error(ret);
            }
            if (ret != ESP_OK) {
                /* A failed hello/wake/listen send must remain retryable. Clear
                 * the local active flag before closing so a stale ONLINE bit
                 * cannot mistake the failed session for a live conversation. */
                xiaozhi_chat_app_set_wakeuped(xiaozhi_chat_app, false);
            }
            if (ret != ESP_OK && audio_opened) {
                esp_xiaozhi_chat_app_audio_error(
                    esp_xiaozhi_chat_close_audio_channel(xiaozhi_chat_app->chat));
            }

            if (ret == ESP_OK) {
                /* The channel hello only establishes the transport. Do not
                 * let the recorder upload until both listen commands have
                 * been accepted by the transport. */
                xiaozhi_chat_app_set_wakeuped(xiaozhi_chat_app, true);
                /* Only a complete hello + wake + listen sequence consumes the
                 * request. If the transport failed, keep it pending for the
                 * next reconnect instead of silently losing the wake. */
                xiaozhi_chat_app_set_wake_pending(xiaozhi_chat_app, false);
                xEventGroupClearBits(xiaozhi_chat_app->data_evt_group,
                                     ESP_XIAOZHI_CHAT_APP_ONLINE);
                wake_retry_delay_ms = XIAOZHI_WAKE_RETRY_INITIAL_MS;
            } else {
                /* WebSocket creation is lazy and may fail before a connected
                 * callback exists. Keep the wake request pending and retry
                 * after the failed session has fully stopped. Back off because
                 * task creation and TLS setup both need scarce internal RAM. */
                ESP_LOGW(TAG, "Xiaozhi wake session unavailable; retrying in %u ms",
                         (unsigned)wake_retry_delay_ms);
                vTaskDelay(pdMS_TO_TICKS(wake_retry_delay_ms));
                if (wake_retry_delay_ms < XIAOZHI_WAKE_RETRY_MAX_MS) {
                    wake_retry_delay_ms *= 2U;
                    if (wake_retry_delay_ms > XIAOZHI_WAKE_RETRY_MAX_MS) {
                        wake_retry_delay_ms = XIAOZHI_WAKE_RETRY_MAX_MS;
                    }
                }
                if (!xiaozhi_chat_app_is_wakeuped(xiaozhi_chat_app)) {
                    xEventGroupSetBits(xiaozhi_chat_app->data_evt_group,
                                       ESP_XIAOZHI_CHAT_APP_ONLINE);
                }
            }
        }

        if (bits & ESP_XIAOZHI_CHAT_APP_LISTEN_REARM) {
            xEventGroupClearBits(xiaozhi_chat_app->data_evt_group,
                                 ESP_XIAOZHI_CHAT_APP_LISTEN_REARM);

            if (xiaozhi_chat_app_is_wakeuped(xiaozhi_chat_app) &&
                    xiaozhi_chat_app_is_transport_connected(xiaozhi_chat_app) &&
                    xiaozhi_chat_app->chat != 0) {
                xiaozhi_tts_wait_for_drain();
                if (xiaozhi_chat_app_is_wakeuped(xiaozhi_chat_app) &&
                        xiaozhi_chat_app_is_transport_connected(xiaozhi_chat_app)) {
                    /* Frames captured before TTS stop belong to the previous
                     * turn. Drop them before the next listen/start command. */
                    xiaozhi_audio_send_clear();
                    /* Upstream sends a bare listen/start when the TTS turn
                     * ends (Application::StartListeningAudio); the abort is
                     * only for a real wake-word interrupt. This is safe to
                     * evaluate now that the VAD barge-in churn during TTS has
                     * been removed, so the server sees a clean Speaking ->
                     * Listening transition. */
                    ESP_LOGI(TAG, "listen rearm: sending listen start");
                    esp_err_t ret = esp_xiaozhi_chat_send_start_listening(
                        xiaozhi_chat_app->chat,
                        ESP_XIAOZHI_CHAT_LISTENING_MODE_AUTO);
                    esp_xiaozhi_chat_app_audio_error(ret);
                    if (ret == ESP_OK) {
                        /* Server is listening again: resume PCM production.
                         * Enabling before this point would queue speech the
                         * server is not yet accepting. */
                        xiaozhi_afe_set_voice_output(true);
                        s_log_first_audio_frame = true;
                        xSemaphoreTake(xiaozhi_chat_app->state_mutex, portMAX_DELAY);
                        xiaozhi_chat_app->listen_rearm_pending = false;
                        xSemaphoreGive(xiaozhi_chat_app->state_mutex);
                        ESP_LOGI(TAG, "listen rearm: server listening again");
                    } else {
                        xiaozhi_chat_app_set_wakeuped(xiaozhi_chat_app, false);
                        xEventGroupSetBits(xiaozhi_chat_app->data_evt_group,
                                           ESP_XIAOZHI_CHAT_APP_OFFLINE);
                    }
                } else {
                    xiaozhi_chat_app_set_wakeuped(xiaozhi_chat_app, false);
                }
            } else {
                xSemaphoreTake(xiaozhi_chat_app->state_mutex, portMAX_DELAY);
                xiaozhi_chat_app->listen_rearm_pending = false;
                xSemaphoreGive(xiaozhi_chat_app->state_mutex);
            }
        }

        if (bits & ESP_XIAOZHI_CHAT_APP_AUDIO_SEND) {
            xEventGroupClearBits(xiaozhi_chat_app->data_evt_group,
                                 ESP_XIAOZHI_CHAT_APP_AUDIO_SEND);
            if (xiaozhi_chat_app_is_wakeuped(xiaozhi_chat_app) &&
                    xiaozhi_chat_app_is_transport_connected(xiaozhi_chat_app) &&
                    xiaozhi_chat_app->chat != 0 &&
                    !xiaozhi_chat_app_is_tts_playing(xiaozhi_chat_app) &&
                    !xiaozhi_chat_app_is_listen_rearm_pending(xiaozhi_chat_app)) {
                xiaozhi_audio_send_drain(xiaozhi_chat_app);
            } else {
                xiaozhi_audio_send_clear();
            }
        }
    }
}

static void esp_xiaozhi_chat_app_audio_read(void *pv)
{
    int ret = 0;
    int consecutive_errors = 0;
    int64_t held_log_t0 = 0;
    esp_xiaozhi_chat_app_t *xiaozhi_chat_app = (esp_xiaozhi_chat_app_t *)pv;
    uint8_t *data = esp_gmf_oal_calloc(1, ESP_XIAOZHI_CHAT_REC_READ_SIZE);
    if (data == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for audio data");
        vTaskDelete(NULL);
        return;
    }

    while (true) {
        ret = audio_recorder_read_data(data, ESP_XIAOZHI_CHAT_REC_READ_SIZE);
        if (ret > 0) {
            consecutive_errors = 0;
        }

        if (ret > 0 && xiaozhi_chat_app_is_wakeuped(xiaozhi_chat_app) &&
                !xiaozhi_chat_app_is_transport_connected(xiaozhi_chat_app)) {
            /* The transport can transition to disconnected between the
             * WebSocket event and this recorder task. Stop the active audio
             * session immediately so it cannot spin on invalid-state sends. */
            xiaozhi_chat_app_set_wakeuped(xiaozhi_chat_app, false);
            xiaozhi_chat_app->audio_send_errors = 0;
            if (xiaozhi_chat_app->data_evt_group != NULL) {
                xEventGroupSetBits(xiaozhi_chat_app->data_evt_group,
                                   ESP_XIAOZHI_CHAT_APP_OFFLINE);
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (ret > 0 && !xiaozhi_chat_app_is_wakeuped(xiaozhi_chat_app)) {
            /* Keep the last ~3 s of encoded audio while idle so the wake
             * handshake can stream the utterance that contained the wake
             * word, matching upstream's wake-word audio cache. */
            xiaozhi_preroll_capture(xiaozhi_chat_app, data, (size_t)ret);
        }

        if (ret > 0 && xiaozhi_chat_app_is_wakeuped(xiaozhi_chat_app) &&
                xiaozhi_chat_app_is_transport_connected(xiaozhi_chat_app) &&
                !xiaozhi_chat_app_is_audio_send_blocked(xiaozhi_chat_app) &&
                xiaozhi_chat_app->chat != 0 &&
                !xiaozhi_chat_app_is_tts_playing(xiaozhi_chat_app) &&
                !xiaozhi_chat_app_is_listen_rearm_pending(xiaozhi_chat_app)) {
            if (!xiaozhi_audio_send_enqueue(xiaozhi_chat_app, data, (size_t)ret)) {
                ESP_LOGW(TAG, "Failed to enqueue microphone frame; dropping it");
            }
            continue;
        }

        /* A wakeup session is open but this frame was not queued: surface the
         * exact gate once per second instead of silently discarding the user's
         * speech (this is what made a first-turn reply look "ignored"). */
        if (ret > 0 && xiaozhi_chat_app_is_wakeuped(xiaozhi_chat_app) &&
                xiaozhi_chat_app_is_transport_connected(xiaozhi_chat_app)) {
            const int64_t now_ms = esp_timer_get_time() / 1000;
            if (now_ms - held_log_t0 >= 1000) {
                held_log_t0 = now_ms;
                ESP_LOGW(TAG,
                         "mic frames held: blocked=%d tts=%d rearm=%d",
                         xiaozhi_chat_app_is_audio_send_blocked(xiaozhi_chat_app) ? 1 : 0,
                         xiaozhi_chat_app_is_tts_playing(xiaozhi_chat_app) ? 1 : 0,
                         xiaozhi_chat_app_is_listen_rearm_pending(xiaozhi_chat_app) ? 1 : 0);
            }
        }

        if (ret <= 0) {
            consecutive_errors++;
            if (consecutive_errors == 1 ||
                    (consecutive_errors % ESP_XIAOZHI_CHAT_APP_AUDIO_READ_ERROR_LOG_INTERVAL) == 0) {
                ESP_LOGW(TAG, "audio_recorder_read_data failed: %d (consecutive=%d)", ret, consecutive_errors);
            }
            if (consecutive_errors >= ESP_XIAOZHI_CHAT_APP_AUDIO_READ_MAX_ERRORS) {
                ESP_LOGE(TAG, "audio recorder read failed too many times, stopping audio read thread");
                xiaozhi_chat_app_set_wakeuped(xiaozhi_chat_app, false);
                if (xiaozhi_chat_app->data_evt_group != NULL) {
                    xEventGroupSetBits(xiaozhi_chat_app->data_evt_group, ESP_XIAOZHI_CHAT_APP_OFFLINE);
                }
                esp_xiaozhi_chat_display_set_status("Recorder Error");
                esp_xiaozhi_chat_display_set_notification("Recorder Error", 2000);
                esp_xiaozhi_chat_display_set_emotion("sad");
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(ESP_XIAOZHI_CHAT_APP_AUDIO_READ_ERROR_BACKOFF_MS));
        }
    }

    esp_gmf_oal_free(data);
    xiaozhi_chat_app->read_thread = NULL;
    vTaskDelete(NULL);
}

/**
 * @brief Read a few raw blocks from the record codec and log their level.
 *
 * Runs once, right after the record device is opened and before the recorder
 * pipeline claims it. A peak of 0 on every channel means the ES8389 ADC is not
 * delivering samples at all (I2S RX / PGA / mic wiring); a healthy level here
 * means the problem is further up in AFE/WakeNet.
 */
static void esp_xiaozhi_chat_app_mic_selftest(esp_codec_dev_handle_t rec_dev, int sample_rate, int channels)
{
    if (rec_dev == NULL || channels <= 0 || channels > 2 || sample_rate <= 0) {
        return;
    }

    const int frames = sample_rate / 10;  /* 100 ms per block */
    const size_t bytes = (size_t)frames * (size_t)channels * sizeof(int16_t);
    int16_t *buf = (int16_t *)esp_gmf_oal_calloc(1, bytes);
    if (buf == NULL) {
        ESP_LOGW(TAG, "Mic self-test: out of memory");
        return;
    }

    /* First block after open is usually garbage while the ADC settles. */
    esp_codec_dev_read(rec_dev, buf, bytes);

    /* Diagnostic: read back the ES8389 ADC / PGA / analog registers so we can
     * tell "wired wrong" from "bias off" from "muted". peak==0 with a healthy
     * register map means the I2S RX clock is missing; peak==0 with bias bit
     * clear means the electret mics are unpowered. */
    static const int dbg_regs[] = {0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
                                   0x61, 0x62, 0x63, 0x64, 0x71, 0x72, 0x73};
    char regdump[160];
    int off = 0;
    for (size_t i = 0; i < sizeof(dbg_regs) / sizeof(dbg_regs[0]); i++) {
        int v = -1;
        esp_codec_dev_read_reg(rec_dev, dbg_regs[i], &v);
        off += snprintf(regdump + off, sizeof(regdump) - off, "0x%02x=%02d ", dbg_regs[i], v < 0 ? -1 : v);
    }
    ESP_LOGI(TAG, "Mic self-test ES8389 regs: %s", regdump);

    /* The ES8389 open sequence now powers MICBIAS (0x62 bit7) for this board, so
     * a healthy register map plus the dump above should show 0x62=0x80 and the
     * three blocks below should report non-zero peaks on both channels. */
    for (int blk = 0; blk < ESP_XIAOZHI_CHAT_APP_MIC_SELFTEST_BLOCKS; blk++) {
        int r = esp_codec_dev_read(rec_dev, buf, bytes);
        if (r != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "Mic self-test: read failed (%d)", r);
            break;
        }
        int32_t peak[2] = {0, 0};
        int64_t energy[2] = {0, 0};
        for (int i = 0; i < frames; i++) {
            for (int c = 0; c < channels; c++) {
                int32_t v = buf[i * channels + c];
                int32_t a = v < 0 ? -v : v;
                if (a > peak[c]) {
                    peak[c] = a;
                }
                energy[c] += (int64_t)v * v;
            }
        }
        if (channels == 2) {
            ESP_LOGI(TAG, "Mic self-test blk%d: ch0 peak=%d rms=%d | ch1 peak=%d rms=%d",
                     blk, (int)peak[0], (int)(energy[0] / frames > 0 ? (int)sqrt((double)(energy[0] / frames)) : 0),
                     (int)peak[1], (int)(energy[1] / frames > 0 ? (int)sqrt((double)(energy[1] / frames)) : 0));
        } else {
            ESP_LOGI(TAG, "Mic self-test blk%d: ch0 peak=%d rms=%d",
                     blk, (int)peak[0], (int)(energy[0] / frames > 0 ? (int)sqrt((double)(energy[0] / frames)) : 0));
        }
    }

    /* Mic-path fix is applied permanently in the ES8389 driver (es8389.c /
     * es8389_reg.h): 0x62 bias voltage = 0x87 (non-zero; the 0x80 default left
     * v=0 and starved the electret) and 0x72 input mux = single-ended MIC1P
     * (mux 5). This board wires the mic single-ended, not as the differential
     * MIC1P-MIC1N pair the upstream default selected, so the differential mux
     * common-mode-rejected the signal and WakeNet never saw it. */

    esp_gmf_oal_free(buf);
}

/* Network-independent audio self-test. Opens the playback and record codec
 * devices, enables the XL9555 amplifier output, plays a short boot chime to
 * prove the speaker path, reads back the DAC volume/mute registers and the PA
 * level, then captures a few mic blocks to prove the ADC path. Both devices are
 * closed afterwards so the full audio pipeline (esp_xiaozhi_chat_app_audio)
 * can re-open them cleanly. Runs on every boot, before WiFi/provisioning, so
 * the "no sound / no mic" fixes are proven locally without any network. */
static void esp_xiaozhi_chat_app_audio_selftest(esp_board_manager_adapter_info_t *bsp_info)
{
    if (bsp_info == NULL || bsp_info->play_dev == NULL || bsp_info->rec_dev == NULL) {
        ESP_LOGW(TAG, "[AUDIO_SELFTEST] skipped (no codec devices)");
        return;
    }
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = bsp_info->sample_rate,
        .channel = bsp_info->channels,
        .bits_per_sample = bsp_info->sample_bits,
    };
    esp_codec_dev_handle_t play_dev = (esp_codec_dev_handle_t)bsp_info->play_dev;
    esp_codec_dev_handle_t rec_dev = (esp_codec_dev_handle_t)bsp_info->rec_dev;

    ESP_LOGI(TAG, "[AUDIO_SELFTEST] Opening playback codec (rate=%d bits=%d ch=%d)",
             bsp_info->sample_rate, bsp_info->sample_bits, bsp_info->channels);
    if (esp_codec_dev_open(play_dev, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "[AUDIO_SELFTEST] Failed to open playback codec");
        return;
    }
    esp_err_t speaker_ret = tbao_s31_board_set_speaker(true);

    if (current_volume == 0) {
        esp_codec_dev_set_out_mute(play_dev, true);
    } else {
        esp_codec_dev_set_out_mute(play_dev, false);
        esp_codec_dev_set_out_vol(play_dev, current_volume);
    }
    app_sfx_set_play_dev(play_dev, bsp_info->sample_rate);
    app_sfx_play(APP_SFX_BOOT);

    int rb_vol = -1, rb_mute = -1;
    esp_codec_dev_read_reg(play_dev, 0x46, &rb_vol);
    esp_codec_dev_read_reg(play_dev, 0x20, &rb_mute);
    ESP_LOGI(TAG, "[AUDIO_SELFTEST] XL9555_P07 ret=%s  DAC_VOL_REG0x46=%d  DAC_MUTE_REG0x20=0x%02x (bit0/1=mute) out_vol_req=%d",
             esp_err_to_name(speaker_ret), rb_vol, rb_mute < 0 ? 0 : (uint8_t)rb_mute,
             current_volume);

    ESP_LOGI(TAG, "[AUDIO_SELFTEST] Opening record codec");
    if (esp_codec_dev_open(rec_dev, &fs) == ESP_CODEC_DEV_OK) {
        esp_codec_dev_set_in_gain(rec_dev, 24.0);
        esp_xiaozhi_chat_app_mic_selftest(rec_dev, bsp_info->sample_rate, bsp_info->channels);
        esp_codec_dev_close(rec_dev);
    } else {
        ESP_LOGE(TAG, "[AUDIO_SELFTEST] Failed to open record codec");
    }
    esp_codec_dev_close(play_dev);
    /* Reset the app_sfx global so a tap before the full audio pipeline opens
     * the device again can't write to this now-closed handle. */
    app_sfx_set_play_dev(NULL, 0);
    ESP_LOGI(TAG, "[AUDIO_SELFTEST] done (devices closed)");
}

static esp_err_t esp_xiaozhi_chat_app_audio(esp_xiaozhi_chat_app_t *xiaozhi_chat_app, esp_board_manager_adapter_info_t bsp_info)
{
    ESP_RETURN_ON_FALSE(xiaozhi_chat_app != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid app");
    ESP_RETURN_ON_FALSE(bsp_info.play_dev != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid playback device");
    ESP_RETURN_ON_FALSE(bsp_info.rec_dev != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid record device");

    memset(&s_audio_runtime, 0, sizeof(s_audio_runtime));
    xiaozhi_chat_app->read_thread = NULL;
    xiaozhi_chat_app->audio_channel = NULL;

    av_processor_afe_config_t afe_config = DEFAULT_AV_PROCESSOR_AFE_CONFIG();
    afe_config.ai_mode_wakeup = true;

    audio_manager_config_t config = DEFAULT_AUDIO_MANAGER_CONFIG();
    esp_err_t ret = ESP_OK;

    config.play_dev = bsp_info.play_dev;
    config.rec_dev = bsp_info.rec_dev;
    s_play_dev = (esp_codec_dev_handle_t)bsp_info.play_dev;
    /* The ES8389 is opened with no_dac_ref == false, so the codec emits
     * "ADCL + DACR" ("Set internal reference signal" in the driver log):
     * channel 0 is the microphone, channel 1 is an INTERNAL loopback of the DAC
     * output, not a second microphone. That is precisely the "MR" layout the
     * board adapter derives from the YAML, and it is what lets the AFE run AEC
     * so the device does not hear (and wake on) its own TTS. Forcing "MM" here
     * both disabled AEC ("no reference channel in mic layout") and fed the
     * beamformer the speaker signal, so prefer the adapter's layout. */
    const char *mic_layout = (bsp_info.mic_layout[0] != '\0')
        ? (const char *)bsp_info.mic_layout
        : ((bsp_info.channels >= 2) ? ESP_XIAOZHI_CHAT_APP_MIC_LAYOUT_DUAL
                                    : ESP_XIAOZHI_CHAT_APP_MIC_LAYOUT_SINGLE);
    snprintf(config.mic_layout, sizeof(config.mic_layout), "%s", mic_layout);
    ESP_LOGI(TAG, "AFE mic layout: \"%s\" (%d capture ch, board adapter suggested \"%.*s\")",
             config.mic_layout, bsp_info.channels, 8, bsp_info.mic_layout);
    config.board_sample_rate = bsp_info.sample_rate;
    config.board_bits = bsp_info.sample_bits;
    config.board_channels = bsp_info.channels;
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = config.board_sample_rate,
        .channel = config.board_channels,
        .bits_per_sample = config.board_bits,
    };
    audio_playback_config_t playback_config = DEFAULT_AUDIO_PLAYBACK_CONFIG();
    av_processor_encoder_config_t recorder_cfg = {0};
    audio_recorder_config_t recorder_config = DEFAULT_AUDIO_RECORDER_CONFIG();
    av_processor_decoder_config_t feeder_cfg = {0};
    audio_feeder_config_t feeder_config = DEFAULT_AUDIO_FEEDER_CONFIG();

    do {
        ret = audio_manager_init(&config);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to init audio manager: %s", esp_err_to_name(ret));
            break;
        }
        s_audio_runtime.audio_manager_inited = true;

        if (esp_codec_dev_open(config.play_dev, &fs) != ESP_CODEC_DEV_OK) {
            ret = ESP_FAIL;
            ESP_LOGE(TAG, "Failed to open playback codec device");
            break;
        }
        s_audio_runtime.play_dev_opened = true;

        esp_err_t speaker_ret = tbao_s31_board_set_speaker(true);
        ESP_LOGI(TAG, "[AUDIO] XL9555_P07 amplifier enable ret=%s",
                 esp_err_to_name(speaker_ret));

        if (esp_codec_dev_open(config.rec_dev, &fs) != ESP_CODEC_DEV_OK) {
            ret = ESP_FAIL;
            ESP_LOGE(TAG, "Failed to open record codec device");
            break;
        }
        s_audio_runtime.rec_dev_opened = true;

        /* Gain MUST be applied AFTER the codec is open. es8389_apply_mic_pga_gain()
         * silently drops the call while is_open == false, so setting it before
         * esp_codec_dev_open() was a no-op (mic self-test read all zeros). */
        if (current_volume == 0) {
            esp_codec_dev_set_out_mute(config.play_dev, true);
        } else {
            esp_codec_dev_set_out_mute(config.play_dev, false);
            if (esp_codec_dev_set_out_vol(config.play_dev, current_volume) != ESP_CODEC_DEV_OK) {
                ret = ESP_FAIL;
                ESP_LOGE(TAG, "Failed to set output volume");
                break;
            }
        }
        if (esp_codec_dev_set_in_gain(config.rec_dev, 24.0) != ESP_CODEC_DEV_OK) {
            ret = ESP_FAIL;
            ESP_LOGE(TAG, "Failed to set input gain");
            break;
        }

        /* Play a short self-test chime to prove the playback path works end to
         * end (codec + PA + speaker). Harmless if it glitches against the still
         * idle audio manager. */
        app_sfx_set_play_dev(s_play_dev, config.board_sample_rate);
        app_sfx_play(APP_SFX_BOOT);

        /* SELF-TEST READBACK: confirm the codec actually left the DAC unmuted
         * with non-zero volume and the amplifier output was enabled. */
        int rb_vol = -1, rb_mute = -1;
        esp_codec_dev_read_reg(config.play_dev, 0x46, &rb_vol);
        esp_codec_dev_read_reg(config.play_dev, 0x20, &rb_mute);
        ESP_LOGI(TAG, "[AUDIO_SELFTEST] XL9555_P07 ret=%s  DAC_VOL_REG0x46=%d  DAC_MUTE_REG0x20=0x%02x (bit0/1=mute)  out_vol_req=%d",
                 esp_err_to_name(speaker_ret), rb_vol, rb_mute < 0 ? 0 : (uint8_t)rb_mute,
                 current_volume);

        esp_xiaozhi_chat_app_mic_selftest(config.rec_dev, config.board_sample_rate, config.board_channels);

        /* Initialise the playback DSP chain (ESP-Audio-Effects). av_processor
         * calls app_audio_dsp_playback_run() right before writing each decoded
         * frame to the codec. */
        if (app_audio_dsp_init_playback(config.board_sample_rate,
                                         config.board_channels,
                                         config.board_bits) != ESP_OK) {
            ESP_LOGW(TAG, "Playback DSP init failed (continuing without DSP)");
        } else {
            s_audio_runtime.dsp_inited = true;
        }
        /* Network music player: routes decoded PCM to the same codec. */
        if (app_music_player_init((esp_codec_dev_handle_t)config.play_dev,
                                  config.board_sample_rate) != ESP_OK) {
            ESP_LOGW(TAG, "Music player init failed (continuing)");
        } else {
            s_audio_runtime.music_player_inited = true;
        }
        app_audio_bargein_register_interrupt_cb(esp_xiaozhi_chat_app_bargein_interrupt);
        app_audio_bargein_reset();

        ret = audio_playback_open(&playback_config);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to open audio playback: %s", esp_err_to_name(ret));
            break;
        }
        s_audio_runtime.playback_opened = true;

        recorder_cfg.format = AV_PROCESSOR_FORMAT_ID_OPUS;
        recorder_cfg.params.opus.audio_info.sample_rate = xiaozhi_chat_app->audio.sample_rate;
        recorder_cfg.params.opus.audio_info.sample_bits = 16;
        recorder_cfg.params.opus.audio_info.channels = xiaozhi_chat_app->audio.channels;
        recorder_cfg.params.opus.audio_info.frame_duration = xiaozhi_chat_app->audio.frame_duration;
        recorder_cfg.params.opus.enable_vbr = false;
        recorder_cfg.params.opus.bitrate = 24000;

        recorder_config.encoder_cfg = recorder_cfg;
        recorder_config.afe_config = afe_config;
        recorder_config.recorder_event_cb = esp_xiaozhi_chat_app_audio_recorder;
        recorder_config.recorder_ctx = (void *)xiaozhi_chat_app;
        /* Mic energy meter for the barge-in controller (runs before AFE). */
        recorder_config.input_cb = app_audio_dsp_mic_input_cb;
        recorder_config.input_ctx = NULL;
        recorder_config.recorder_task_config.task_core = 1;
        /* The AFE fetch task must NOT share core 1 with the GMF recorder
         * task: when the AFE(FEED) ringbuffer fills, the recorder (prio 10)
         * busy-waits and permanently starves fetch (prio 5) on the same core.
         * The pipeline then livelocks until the task watchdog fires and the
         * device goes deaf mid-conversation. Core 0 round-robins fetch with
         * its same-priority peers, so the drain always makes progress. */
        recorder_config.afe_fetch_task_config.task_core = 0;
        ret = audio_recorder_open(&recorder_config);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to open audio recorder: %s", esp_err_to_name(ret));
            break;
        }
        s_audio_runtime.recorder_opened = true;
        if (audio_recorder_get_afe_element_handle(&s_afe_element) != ESP_OK) {
            s_afe_element = NULL;
            ESP_LOGW(TAG, "AFE element unavailable; TTS voice-output gating disabled");
        }

        feeder_cfg.format = AV_PROCESSOR_FORMAT_ID_OPUS;
        feeder_cfg.params.opus.audio_info.sample_rate = xiaozhi_chat_app->audio.sample_rate;
        feeder_cfg.params.opus.audio_info.sample_bits = 16;
        feeder_cfg.params.opus.audio_info.channels = xiaozhi_chat_app->audio.channels;
        feeder_cfg.params.opus.audio_info.frame_duration = xiaozhi_chat_app->audio.frame_duration;
        /* The server's TTS Opus is 24 kHz; this decoder is opened at the local
         * 16 kHz rate and libopus resamples internally.  The pipeline then
         * converts mono -> stereo / 16 kHz for the codec. */
        ESP_LOGI(TAG, "TTS decode: opus %d Hz %d ch %d ms -> codec %d Hz %d ch %d bit",
                 feeder_cfg.params.opus.audio_info.sample_rate,
                 feeder_cfg.params.opus.audio_info.channels,
                 feeder_cfg.params.opus.audio_info.frame_duration,
                 config.board_sample_rate, config.board_channels, config.board_bits);

        feeder_config.feeder_task_config.task_core = 1;
        feeder_config.decoder_cfg = feeder_cfg;
        s_audio_runtime.feeder_config = feeder_config;
        ret = audio_feeder_open(&s_audio_runtime.feeder_config);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to open audio feeder: %s", esp_err_to_name(ret));
            break;
        }
        s_audio_runtime.feeder_opened = true;

        ret = audio_feeder_run();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to run audio feeder: %s", esp_err_to_name(ret));
            break;
        }
        s_audio_runtime.feeder_running = true;

        ret = xiaozhi_tts_feed_init();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize asynchronous TTS feed: %s",
                     esp_err_to_name(ret));
            break;
        }

        ret = xiaozhi_audio_send_init();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize asynchronous audio send queue: %s",
                     esp_err_to_name(ret));
            break;
        }

        if (!xiaozhi_preroll_init()) {
            ESP_LOGW(TAG, "Wake preroll buffer unavailable; continuing without it");
        }

        if (esp_gmf_oal_thread_create(&xiaozhi_chat_app->read_thread, "audio_read",
                                      esp_xiaozhi_chat_app_audio_read, (void *)xiaozhi_chat_app,
                                      3096, 12, false, 1) != ESP_GMF_ERR_OK) {
            ret = ESP_FAIL;
            ESP_LOGE(TAG, "Failed to create audio read thread");
            break;
        }

        if (esp_gmf_oal_thread_create(&xiaozhi_chat_app->audio_channel, "audio_channel",
                                      esp_xiaozhi_chat_app_audio_channel, (void *)xiaozhi_chat_app,
                                      3096, 12, false, 1) != ESP_GMF_ERR_OK) {
            ret = ESP_FAIL;
            ESP_LOGE(TAG, "Failed to create audio channel thread");
            break;
        }
    } while (0);

    if (ret != ESP_OK) {
        xiaozhi_audio_release(xiaozhi_chat_app, &bsp_info);
    }

    return ret;
}

static esp_err_t xiaozhi_audio_release(esp_xiaozhi_chat_app_t *xiaozhi_chat_app,
                                       const esp_board_manager_adapter_info_t *bsp_info)
{
    esp_err_t first_error = ESP_OK;

    if (xiaozhi_chat_app != NULL) {
        xiaozhi_chat_app_set_wakeuped(xiaozhi_chat_app, false);
        xiaozhi_chat_app_set_wake_pending(xiaozhi_chat_app, false);
        xiaozhi_chat_app_set_audio_send_blocked(xiaozhi_chat_app, true);
        if (xiaozhi_chat_app->state_mutex != NULL) {
            xSemaphoreTake(xiaozhi_chat_app->state_mutex, portMAX_DELAY);
            xiaozhi_chat_app->transport_connected = false;
            xSemaphoreGive(xiaozhi_chat_app->state_mutex);
        }
        xiaozhi_chat_app->audio_send_errors = 0;
        if (xiaozhi_chat_app->data_evt_group != NULL) {
            xEventGroupClearBits(xiaozhi_chat_app->data_evt_group,
                                 ESP_XIAOZHI_CHAT_APP_AUDIO_SEND |
                                 ESP_XIAOZHI_CHAT_APP_ONLINE |
                                 ESP_XIAOZHI_CHAT_APP_OFFLINE);
        }
    }

    /* Stop new direct SFX writes before the codec handle is closed. */
    if (s_play_dev != NULL) {
        app_sfx_set_play_dev(NULL, 0);
    }
    app_audio_bargein_register_interrupt_cb(NULL);
    app_audio_bargein_reset();
    xiaozhi_tts_feed_shutdown();

    if (xiaozhi_chat_app != NULL && xiaozhi_chat_app->audio_channel != NULL) {
        if (esp_gmf_oal_thread_delete(xiaozhi_chat_app->audio_channel) != ESP_GMF_ERR_OK &&
            first_error == ESP_OK) {
            first_error = ESP_FAIL;
        }
        xiaozhi_chat_app->audio_channel = NULL;
    }
    if (xiaozhi_chat_app != NULL && xiaozhi_chat_app->read_thread != NULL) {
        if (esp_gmf_oal_thread_delete(xiaozhi_chat_app->read_thread) != ESP_GMF_ERR_OK &&
            first_error == ESP_OK) {
            first_error = ESP_FAIL;
        }
        xiaozhi_chat_app->read_thread = NULL;
    }

    /* The producer and serialized sender are stopped before their slot queues
     * are destroyed. */
    xiaozhi_audio_send_release();
    xiaozhi_preroll_deinit();

    /* The music player has its own decoder task and must go away before the
     * shared codec device is closed. */
    if (s_audio_runtime.music_player_inited || s_play_dev != NULL) {
        esp_err_t ret = app_music_player_deinit();
        if (ret != ESP_OK && first_error == ESP_OK) {
            first_error = ret;
        }
    }
    if (s_audio_runtime.feeder_running) {
        esp_err_t ret = audio_feeder_stop();
        if (ret != ESP_OK && first_error == ESP_OK) {
            first_error = ret;
        }
        s_audio_runtime.feeder_running = false;
    }
    if (s_audio_runtime.feeder_opened) {
        esp_err_t ret = audio_feeder_close();
        if (ret != ESP_OK && first_error == ESP_OK) {
            first_error = ret;
        }
    }
    if (s_audio_runtime.recorder_opened) {
        s_afe_element = NULL;
        esp_err_t ret = audio_recorder_close();
        if (ret != ESP_OK && first_error == ESP_OK) {
            first_error = ret;
        }
    }
    if (s_audio_runtime.playback_opened) {
        esp_err_t ret = audio_playback_close();
        if (ret != ESP_OK && first_error == ESP_OK) {
            first_error = ret;
        }
    }
    if (s_audio_runtime.dsp_inited) {
        app_audio_dsp_deinit_playback();
    }

    if (s_audio_runtime.play_dev_opened) {
        esp_err_t ret = tbao_s31_board_set_speaker(false);
        if (ret != ESP_OK && first_error == ESP_OK) {
            first_error = ret;
        }
    }
    if (bsp_info != NULL && s_audio_runtime.rec_dev_opened && bsp_info->rec_dev != NULL) {
        esp_codec_dev_close((esp_codec_dev_handle_t)bsp_info->rec_dev);
    }
    if (bsp_info != NULL && s_audio_runtime.play_dev_opened && bsp_info->play_dev != NULL) {
        esp_codec_dev_close((esp_codec_dev_handle_t)bsp_info->play_dev);
    }
    s_play_dev = NULL;

    if (s_audio_runtime.audio_manager_inited) {
        esp_err_t ret = audio_manager_deinit();
        if (ret != ESP_OK && first_error == ESP_OK) {
            first_error = ret;
        }
    }
    esp_err_t power_ret = tbao_s31_board_set_5v_power(false);
    if (power_ret != ESP_OK && first_error == ESP_OK) {
        first_error = power_ret;
    }

    memset(&s_audio_runtime, 0, sizeof(s_audio_runtime));
    return first_error;
}

static void xiaozhi_camera_release(void)
{
    if (s_camera_explain == NULL) {
        return;
    }

    /* A camera MCP callback may still be waiting for the worker. Serialize
     * destruction with that callback before freeing its HTTP client. */
    if (s_cam_call_mutex != NULL) {
        xSemaphoreTake(s_cam_call_mutex, portMAX_DELAY);
    }
    esp_xiaozhi_camera_destroy(s_camera_explain);
    s_camera_explain = NULL;
    if (s_cam_call_mutex != NULL) {
        xSemaphoreGive(s_cam_call_mutex);
    }
}

static esp_err_t xiaozhi_runtime_enable(void)
{
    if (s_xiaozhi_chat_app == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xiaozhi_chat_app_set_wakeuped(s_xiaozhi_chat_app, false);
    xiaozhi_chat_app_set_wake_pending(s_xiaozhi_chat_app, false);
    if (s_xiaozhi_chat_app->state_mutex != NULL) {
        xSemaphoreTake(s_xiaozhi_chat_app->state_mutex, portMAX_DELAY);
        s_xiaozhi_chat_app->transport_connected = false;
        xSemaphoreGive(s_xiaozhi_chat_app->state_mutex);
    }
    if (s_xiaozhi_chat_app->data_evt_group != NULL) {
        xEventGroupClearBits(s_xiaozhi_chat_app->data_evt_group,
                             ESP_XIAOZHI_CHAT_APP_ONLINE |
                             ESP_XIAOZHI_CHAT_APP_OFFLINE);
    }
    esp_err_t power_ret = tbao_s31_board_set_5v_power(true);
    if (power_ret != ESP_OK) {
        ESP_LOGW(TAG, "T-Bao 5V/audio rail enable failed: %s", esp_err_to_name(power_ret));
    }

    esp_err_t ret = esp_xiaozhi_chat_app_init(s_xiaozhi_chat_app);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable Xiaozhi chat: %s", esp_err_to_name(ret));
        tbao_s31_board_set_5v_power(false);
        return ret;
    }

    if (s_bsp_info.play_dev != NULL && s_bsp_info.rec_dev != NULL) {
        ret = esp_xiaozhi_chat_app_audio(s_xiaozhi_chat_app, s_bsp_info);
        if (ret != ESP_OK) {
            /* Network chat can remain active without local audio hardware. */
            ESP_LOGW(TAG, "Xiaozhi audio init failed; chat remains enabled: %s",
                     esp_err_to_name(ret));
        }
    } else {
        ESP_LOGW(TAG, "Xiaozhi audio devices unavailable; chat enabled without audio");
    }

    esp_xiaozhi_chat_display_set_status("Ready");
    return ESP_OK;
}

static esp_err_t xiaozhi_runtime_disable(void)
{
    if (s_xiaozhi_chat_app == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t first_error = ESP_OK;
    xiaozhi_chat_app_set_wakeuped(s_xiaozhi_chat_app, false);
    xiaozhi_chat_app_set_wake_pending(s_xiaozhi_chat_app, false);

    if (s_xiaozhi_chat_app->chat != 0) {
        esp_err_t ret = esp_xiaozhi_chat_stop(s_xiaozhi_chat_app->chat);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Xiaozhi chat stop failed: %s", esp_err_to_name(ret));
            first_error = ret;
        }
    }

    esp_err_t audio_ret = xiaozhi_audio_release(s_xiaozhi_chat_app, &s_bsp_info);
    if (audio_ret != ESP_OK && first_error == ESP_OK) {
        first_error = audio_ret;
    }

    if (s_xiaozhi_chat_app->chat != 0) {
        esp_err_t ret = esp_xiaozhi_chat_deinit(s_xiaozhi_chat_app->chat);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Xiaozhi chat deinit failed: %s", esp_err_to_name(ret));
            if (first_error == ESP_OK) {
                first_error = ret;
            }
        } else {
            s_xiaozhi_chat_app->chat = 0;
        }
    }

    xiaozhi_camera_release();

    esp_xiaozhi_chat_display_set_status("AI Disabled");
    return first_error;
}

static void xiaozhi_runtime_task(void *arg)
{
    (void)arg;

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        /* Requests can change while a network or audio lifecycle operation is
         * in progress. Finish the current operation, then converge on the
         * latest requested state before sleeping again. */
        for (;;) {
            const bool requested = s_runtime_requested;
            esp_err_t ret = ESP_OK;

            if (requested) {
                if (s_runtime_state == ESP_XIAOZHI_CHAT_APP_STATE_ENABLED) {
                    break;
                }
                s_runtime_state = ESP_XIAOZHI_CHAT_APP_STATE_STARTING;
                ret = xiaozhi_runtime_enable();
                s_runtime_last_error = ret;
                if (ret == ESP_OK) {
                    s_runtime_state = ESP_XIAOZHI_CHAT_APP_STATE_ENABLED;
                } else {
                    s_runtime_state = ESP_XIAOZHI_CHAT_APP_STATE_ERROR;
                    break;
                }
            } else {
                if (s_runtime_state == ESP_XIAOZHI_CHAT_APP_STATE_DISABLED) {
                    break;
                }
                s_runtime_state = ESP_XIAOZHI_CHAT_APP_STATE_STOPPING;
                ret = xiaozhi_runtime_disable();
                s_runtime_last_error = ret;
                /* Cleanup is best-effort and always clears the owned handles,
                 * so the externally visible state is disabled even if one
                 * individual close operation reported an error. */
                s_runtime_state = ESP_XIAOZHI_CHAT_APP_STATE_DISABLED;
            }

            if (s_runtime_requested == requested) {
                break;
            }
        }
    }
}

esp_err_t esp_xiaozhi_chat_app_set_enabled(bool enabled)
{
    if (s_xiaozhi_chat_app == NULL || s_runtime_task == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    s_settings.xiaozhi_enabled = enabled;
    if (app_settings_set_xiaozhi_enabled(enabled) != ESP_OK) {
        ESP_LOGW(TAG, "Xiaozhi state changed but could not be persisted");
    }
    s_runtime_requested = enabled;
    if (s_runtime_state != (enabled
                                ? ESP_XIAOZHI_CHAT_APP_STATE_ENABLED
                                : ESP_XIAOZHI_CHAT_APP_STATE_DISABLED)) {
        s_runtime_state = enabled
            ? ESP_XIAOZHI_CHAT_APP_STATE_STARTING
            : ESP_XIAOZHI_CHAT_APP_STATE_STOPPING;
    }
    xTaskNotifyGive(s_runtime_task);
    return ESP_OK;
}

esp_xiaozhi_chat_app_state_t esp_xiaozhi_chat_app_get_state(void)
{
    return s_runtime_state;
}

bool esp_xiaozhi_chat_app_is_enabled(void)
{
    return s_runtime_state == ESP_XIAOZHI_CHAT_APP_STATE_ENABLED;
}

esp_err_t esp_xiaozhi_chat_app(void)
{
    esp_log_level_set("*", ESP_LOG_INFO);

    esp_err_t settings_ret = app_settings_load(&s_settings);
    if (settings_ret != ESP_OK) {
        ESP_LOGW(TAG, "Using default settings after load failure: %s",
                 esp_err_to_name(settings_ret));
    }
    esp_err_t language_ret = app_language_init();
    if (language_ret != ESP_OK) {
        ESP_LOGW(TAG, "Using default language after load failure: %s",
                 esp_err_to_name(language_ret));
    }
    app_sfx_set_enabled(s_settings.ui_sounds_enabled);
    current_volume = s_settings.volume;
    current_brightness = s_settings.brightness;
    s_runtime_requested = s_settings.xiaozhi_enabled;
    s_runtime_state = s_settings.xiaozhi_enabled
        ? ESP_XIAOZHI_CHAT_APP_STATE_STARTING
        : ESP_XIAOZHI_CHAT_APP_STATE_DISABLED;
    s_runtime_last_error = ESP_OK;
    const bool xiaozhi_enabled = s_runtime_requested;

    esp_xiaozhi_chat_app_t *xiaozhi_chat_app = (esp_xiaozhi_chat_app_t *)calloc(1, sizeof(esp_xiaozhi_chat_app_t));
    ESP_RETURN_ON_FALSE(xiaozhi_chat_app, ESP_ERR_NO_MEM, TAG, "Failed to allocate memory for Xiaozhi chat app");
    esp_err_t ret = ESP_OK;
    bool evt_group_created = false;
    bool state_mutex_created = false;
    bool event_registered = false;
    bool board_inited = false;
    esp_board_manager_adapter_info_t bsp_info = {0};

    do {
        xiaozhi_chat_app->data_evt_group = xEventGroupCreate();
        if (xiaozhi_chat_app->data_evt_group == NULL) {
            ret = ESP_ERR_NO_MEM;
            ESP_LOGE(TAG, "Failed to create event group");
            break;
        }
        evt_group_created = true;

        xiaozhi_chat_app->state_mutex = xSemaphoreCreateMutex();
        if (xiaozhi_chat_app->state_mutex == NULL) {
            ret = ESP_ERR_NO_MEM;
            ESP_LOGE(TAG, "Failed to create chat app state mutex");
            break;
        }
        state_mutex_created = true;

        ret = esp_event_handler_register(ESP_XIAOZHI_CHAT_EVENTS, ESP_EVENT_ANY_ID, esp_xiaozhi_chat_app_event, xiaozhi_chat_app);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register chat app event handler: %s", esp_err_to_name(ret));
            break;
        }
        event_registered = true;

        xiaozhi_chat_app->audio = (esp_xiaozhi_chat_audio_t) {
            .format = "opus",
            .sample_rate = 16000,
            .channels = 1,
            .frame_duration = 60,
        };

        esp_board_manager_adapter_config_t bsp_config = ESP_BOARD_MANAGER_ADAPTER_CONFIG_DEFAULT();
        bsp_config.enable_lcd = true;
        bsp_config.enable_lcd_backlight = true;
        bsp_config.enable_lvgl = false;
        /* T-Bao S31 audio is on the Board Manager primary I2C bus
         * (SDA=GPIO0/SCL=GPIO1); the board profile owns codec power and the
         * XL9555 amplifier output. */
        bsp_config.enable_audio = true;
        ret = esp_board_manager_adapter_init(&bsp_config, &bsp_info);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to init board manager adapter: %s", esp_err_to_name(ret));
            break;
        }
        board_inited = true;

        ret = tbao_s31_board_set_5v_power(xiaozhi_enabled);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "T-Bao 5V/audio rail setup failed: %s", esp_err_to_name(ret));
        }

        ret = esp_xiaozhi_chat_display_init();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to init display: %s", esp_err_to_name(ret));
            break;
        }

        ret = battery_monitor_start(battery_monitor_display_callback, NULL);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Battery monitor unavailable: %s", esp_err_to_name(ret));
        }
        if (esp_xiaozhi_chat_display_set_brightness(current_brightness) != ESP_OK) {
            ESP_LOGW(TAG, "Failed to apply saved brightness: %d", current_brightness);
        }
        esp_err_t splash_ret = boot_splash_start();
        if (splash_ret != ESP_OK) {
            ESP_LOGW(TAG, "Boot splash unavailable; continuing without it: %s",
                     esp_err_to_name(splash_ret));
        }
        if (xiaozhi_enabled) {
            /* Audio self-test: prove the playback + capture paths work end to
             * end before the chat audio pipeline claims the codec devices. */
            esp_xiaozhi_chat_app_audio_selftest(&bsp_info);
        } else {
            ESP_LOGI(TAG, "Xiaozhi disabled by saved settings; skip chat/audio startup");
        }

        /* Network bring-up: connect to stored WiFi credentials.
         *
         * - No credentials at all (first boot): provisioning_enter() blocks and
         *   shows the SoftAP config page; it reboots on success.
         * - Credentials exist but the AP is unreachable: we wait up to
         *   WIFI_CONNECT_TIMEOUT_MS; if still not joined, we enter provisioning
         *   so the user can (re)configure WiFi. Without the network the AI chat
         *   (emote / conversation) cannot work, so re-pairing is the right move
         *   rather than leaving the board stuck offline.
         *
         * wifi_sta_connect_from_nvs() returns:
         *   ESP_OK               -> joined the stored AP
         *   ESP_ERR_NVS_NOT_FOUND / other NVS errors -> no stored creds
         *   ESP_FAIL             -> creds exist but could not connect within the
         *                            timeout window */
#define WIFI_CONNECT_TIMEOUT_MS 30000   /* 30s: no network -> re-provision */
        esp_err_t wr = wifi_sta_connect_from_nvs(WIFI_CONNECT_TIMEOUT_MS);
        if (wr == ESP_OK) {
            ESP_LOGI(TAG, "WiFi connected to stored AP");
        } else if (wr != ESP_FAIL) {
            /* No stored credentials (first boot / wiped NVS): must provision. */
            ESP_LOGW(TAG, "No stored WiFi credentials (%s); entering provisioning",
                     esp_err_to_name(wr));
            boot_splash_stop();
            provisioning_enter();   /* blocks; reboots on success; never returns */
        } else {
            /* Credentials exist but the AP was unreachable within the timeout:
             * re-enter provisioning so the user can fix the WiFi. */
            ESP_LOGW(TAG, "WiFi unreachable for %d ms; entering provisioning",
                     WIFI_CONNECT_TIMEOUT_MS);
            boot_splash_stop();
            provisioning_enter();   /* blocks; reboots on success; never returns */
        }

        /* Bring up the page UI: persist history, run the page state machine, and
         * start the touch gesture layer (swipes switch pages; taps interact). */
        esp_xiaozhi_chat_app_audio_ctrl_lock_init();

        /* Mount the SD card (4-bit SDIO, GPIO20..25) so music playback and
         * video recording are available. Failure is non-fatal (no card inserted). */
        if (app_sdcard_init() != ESP_OK) {
            ESP_LOGW(TAG, "SD card not available (insert card for music/recording)");
        }
        app_sdcard_ensure_record_dir();

        /* Camera resources are demand-driven. The camera page owns the local
         * preview server and camera-page demand; guard mode and recording keep
         * their own explicit demands. Idle pages do not retain DVP/JPEG work
         * or camera PSRAM buffers. */
        esp_err_t activity_ret = app_activity_init();
        if (activity_ret != ESP_OK) {
            ESP_LOGW(TAG, "Camera activity manager unavailable: %s",
                     esp_err_to_name(activity_ret));
        }
        camera_worker_init();
        app_chat_history_init();
        ui_page_manager_init(s_pages, PAGE_MAX);
        ui_control_center_init();
        esp_err_t motion_ret = motion_behavior_init();
        if (motion_ret != ESP_OK) {
            ESP_LOGW(TAG, "Motion controls unavailable; UI and expressions continue: %s",
                     esp_err_to_name(motion_ret));
        }
        esp_err_t face_follow_ret = app_face_follow_init();
        if (face_follow_ret != ESP_OK) {
            ESP_LOGW(TAG, "Face-follow controls unavailable: %s",
                     esp_err_to_name(face_follow_ret));
        }
        esp_err_t motion_web_ret = motion_web_server_start();
        if (motion_web_ret != ESP_OK) {
            ESP_LOGW(TAG, "Motion web control unavailable: %s",
                     esp_err_to_name(motion_web_ret));
        }
        ui_touch_init();
        ui_touch_set_callback(ui_page_manager_dispatch_gesture);
        ui_page_manager_switch(PAGE_FACE);

        if (xiaozhi_enabled) {
            /* Bring up the network chat. If WiFi/network is unavailable the
             * HTTP fetch fails; instead of tearing the board down (which would
             * blank the screen), keep the display alive and retry periodically.
             * The device auto-connects to the AI as soon as connectivity is
             * present. */
            int chat_attempt = 0;
            while (1) {
                ret = esp_xiaozhi_chat_app_init(xiaozhi_chat_app);
                if (ret == ESP_OK) {
                    break;
                }
                ESP_LOGW(TAG, "Chat app init failed (attempt %d): %s; display stays on, retrying in 5s",
                         ++chat_attempt, esp_err_to_name(ret));
                vTaskDelay(pdMS_TO_TICKS(5000));
            }

            /* Audio is optional; never let it blank the device. */
            ret = esp_xiaozhi_chat_app_audio(xiaozhi_chat_app, bsp_info);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Chat app audio init failed (continuing): %s", esp_err_to_name(ret));
            }
        } else {
            /* Keep the display, Wi-Fi and launcher alive, but do not allocate
             * the chat/MCP or audio pipeline until the user enables Xiaozhi. */
            ESP_LOGI(TAG, "Xiaozhi remains disabled; chat and audio are not started");
            esp_xiaozhi_chat_display_set_status("AI Disabled");
        }

        /* Start the official emote face LAST: its engine + animation buffers
         * claim internal SRAM, and esp-sr's WakeNet (qacc-limited) needs that
         * internal SRAM during AFE creation. Starting emote after the audio /
         * AFE pipeline is up guarantees WakeNet gets the memory it needs, so
         * the wake words keep working. */
        boot_splash_wait();
        esp_err_t emote_ret = esp_xiaozhi_chat_display_enable_emote();
        /* Keep the opaque boot layer in place until the emote renderer has
         * stopped LVGL and taken ownership of the LCD panel. Removing it
         * earlier exposes the light LVGL screen during that handoff. */
        boot_splash_stop();
        /* AFE may have detected a wake word while emote was loading. Release
         * that request only after the renderer (or the LVGL fallback) owns the
         * display, so the first WebSocket session cannot race LCD setup. */
        xiaozhi_chat_app_set_display_ready(xiaozhi_chat_app, true);
        if (emote_ret != ESP_OK) {
            ESP_LOGW(TAG, "Emote unavailable; keeping LVGL UI: %s",
                     esp_err_to_name(emote_ret));
            esp_xiaozhi_chat_display_set_status("Ready");
        }
        if (!xiaozhi_enabled) {
            esp_xiaozhi_chat_display_set_status("AI Disabled");
        }

        /* Keep the board/display context alive for the launcher toggle. The
         * runtime task owns only chat and audio lifecycle transitions. */
        s_xiaozhi_chat_app = xiaozhi_chat_app;
        s_bsp_info = bsp_info;
        s_runtime_requested = xiaozhi_enabled;
        s_runtime_last_error = ESP_OK;
        s_runtime_state = xiaozhi_enabled
            ? ESP_XIAOZHI_CHAT_APP_STATE_ENABLED
            : ESP_XIAOZHI_CHAT_APP_STATE_DISABLED;
        /* This lifecycle path enters cache-disabled I2C/SPI driver sections;
         * the control task stack must therefore remain in internal RAM. */
        BaseType_t runtime_task_ret = xTaskCreatePinnedToCoreWithCaps(
            xiaozhi_runtime_task, "xiaozhi_runtime", XIAOZHI_RUNTIME_TASK_STACK,
            NULL, 4, &s_runtime_task, 1,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (runtime_task_ret != pdPASS) {
            s_runtime_task = NULL;
            s_runtime_state = ESP_XIAOZHI_CHAT_APP_STATE_ERROR;
            s_runtime_last_error = ESP_ERR_NO_MEM;
            ESP_LOGE(TAG, "Failed to create Xiaozhi runtime control task");
        }
        return ESP_OK;
    } while (0);

    if (ret == ESP_OK) {
        return ESP_OK;
    }

    if (xiaozhi_chat_app->chat != 0) {
        esp_err_t cleanup_ret = esp_xiaozhi_chat_deinit(xiaozhi_chat_app->chat);
        if (cleanup_ret == ESP_OK) {
            xiaozhi_chat_app->chat = 0;
        } else {
            ESP_LOGW(TAG, "Failed to clean up chat after startup error: %s",
                     esp_err_to_name(cleanup_ret));
        }
    }
    if (event_registered) {
        esp_event_handler_unregister(ESP_XIAOZHI_CHAT_EVENTS, ESP_EVENT_ANY_ID, esp_xiaozhi_chat_app_event);
    }
    if (board_inited) {
        motion_web_server_stop();
        motion_manager_stop();
        esp_board_manager_adapter_deinit();
    }
    if (evt_group_created) {
        vEventGroupDelete(xiaozhi_chat_app->data_evt_group);
        xiaozhi_chat_app->data_evt_group = NULL;
    }
    if (state_mutex_created && xiaozhi_chat_app->state_mutex != NULL) {
        vSemaphoreDelete(xiaozhi_chat_app->state_mutex);
        xiaozhi_chat_app->state_mutex = NULL;
    }
    free(xiaozhi_chat_app);
    s_runtime_last_error = ret;
    s_runtime_state = ESP_XIAOZHI_CHAT_APP_STATE_ERROR;
    return ret;
}
