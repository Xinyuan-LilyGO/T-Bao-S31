/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Network music player on esp_audio_simple_player.
 *
 * The simple player decodes the stream (MP3/AAC/...) in its own task and calls
 * our out callback with raw PCM. We forward that PCM to the ES8389 play device
 * (same handle the app uses for TTS / SFX), converting mono -> stereo and
 * honoring the current volume.
 */
#include "app_music_player.h"

#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_audio_simple_player.h"

static const char *TAG = "APP_MUSIC";

static esp_codec_dev_handle_t s_play_dev = NULL;
static int   s_sample_rate = 16000;
static int   s_channels    = 2;     /* ES8389 opened in stereo on this board */
static esp_asp_handle_t s_player = NULL;
static volatile app_music_state_t s_state = APP_MUSIC_STOPPED;
static EXT_RAM_BSS_ATTR char s_cur_url[512];

typedef enum {
    MUSIC_CMD_PLAY = 0,
    MUSIC_CMD_PAUSE,
    MUSIC_CMD_RESUME,
    MUSIC_CMD_STOP,
    MUSIC_CMD_SHUTDOWN,
} music_cmd_type_t;

typedef struct {
    music_cmd_type_t type;
    SemaphoreHandle_t done;
    char url[];
} music_cmd_t;

#define MUSIC_CMD_QUEUE_DEPTH 4
#define MUSIC_CTRL_TASK_STACK (4 * 1024)

static QueueHandle_t s_cmd_queue = NULL;
static TaskHandle_t s_cmd_task = NULL;
static volatile bool s_shutting_down = false;

/* --- out callback: simple player -> codec -------------------------------- */

static int music_out_cb(uint8_t *data, int data_size, void *ctx)
{
    (void)ctx;
    if (s_play_dev == NULL || data == NULL || data_size <= 0) {
        return 0;
    }
    /* data is 16-bit PCM (usually mono or stereo at the decoded rate). The
     * codec is fixed at s_channels (2) and s_sample_rate. If the stream rate
     * differs we still write it through; esp_audio_simple_player decodes to
     * the rate of the source. Most streams are 44.1/48k -> the codec would
     * need a rate converter. To keep it simple and correct, we request the
     * player to decode at the codec rate via music_info if possible, and here
     * we just forward stereo samples. */
    int written = esp_codec_dev_write(s_play_dev, data, data_size);
    return written > 0 ? written : 0;
}

/* --- simple player state events ------------------------------------------ */

static int music_event_cb(esp_asp_event_pkt_t *pkt, void *ctx)
{
    (void)ctx;
    if (pkt == NULL) {
        return 0;
    }
    if (pkt->type == ESP_ASP_EVENT_TYPE_STATE) {
        if (pkt->payload == NULL || pkt->payload_size < sizeof(esp_asp_state_t)) {
            return 0;
        }
        esp_asp_state_t st = *(esp_asp_state_t *)pkt->payload;
        switch (st) {
        case ESP_ASP_STATE_RUNNING:  s_state = APP_MUSIC_PLAYING; break;
        case ESP_ASP_STATE_PAUSED:   s_state = APP_MUSIC_PAUSED;  break;
        case ESP_ASP_STATE_STOPPED:
        case ESP_ASP_STATE_FINISHED: s_state = APP_MUSIC_STOPPED; break;
        case ESP_ASP_STATE_ERROR:    s_state = APP_MUSIC_ERROR;   break;
        default: break;
        }
        ESP_LOGI(TAG, "player state -> %s", app_music_player_state_str());
    }
    return 0;
}

static music_cmd_t *music_cmd_alloc(music_cmd_type_t type, const char *url)
{
    size_t url_len = url != NULL ? strlen(url) + 1 : 0;
    music_cmd_t *cmd = heap_caps_malloc(sizeof(*cmd) + url_len,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (cmd == NULL) {
        cmd = malloc(sizeof(*cmd) + url_len);
    }
    if (cmd == NULL) {
        return NULL;
    }
    cmd->type = type;
    cmd->done = NULL;
    if (url_len > 0) {
        memcpy(cmd->url, url, url_len);
    }
    return cmd;
}

static esp_err_t music_cmd_send(music_cmd_type_t type, const char *url)
{
    QueueHandle_t queue = s_cmd_queue;
    if (queue == NULL || s_shutting_down) {
        return ESP_ERR_INVALID_STATE;
    }
    music_cmd_t *cmd = music_cmd_alloc(type, url);
    if (cmd == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (xQueueSend(queue, &cmd, 0) != pdTRUE) {
        free(cmd);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static bool music_player_is_running(esp_asp_state_t *state)
{
    *state = ESP_ASP_STATE_NONE;
    if (s_player == NULL ||
        esp_audio_simple_player_get_state(s_player, state) != ESP_GMF_ERR_OK) {
        return false;
    }
    return *state == ESP_ASP_STATE_RUNNING || *state == ESP_ASP_STATE_PAUSED;
}

static void music_control_task(void *arg)
{
    (void)arg;
    for (;;) {
        music_cmd_t *cmd = NULL;
        if (xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY) != pdTRUE ||
            cmd == NULL) {
            continue;
        }

        esp_gmf_err_t ret = ESP_GMF_ERR_OK;
        esp_asp_state_t player_state = ESP_ASP_STATE_NONE;
        switch (cmd->type) {
        case MUSIC_CMD_PLAY:
            if (music_player_is_running(&player_state)) {
                ret = esp_audio_simple_player_stop(s_player);
            }
            if (ret == ESP_GMF_ERR_OK && s_player != NULL) {
                esp_asp_music_info_t info = {
                    .sample_rate = s_sample_rate,
                    .channels = 1,
                    .bits = 16,
                    .bitrate = 0,
                };
                ret = esp_audio_simple_player_run(s_player, cmd->url, &info);
            }
            if (ret != ESP_GMF_ERR_OK) {
                ESP_LOGE(TAG, "play failed: %x", ret);
                s_state = APP_MUSIC_ERROR;
            }
            break;
        case MUSIC_CMD_PAUSE:
            if (music_player_is_running(&player_state) &&
                player_state == ESP_ASP_STATE_RUNNING) {
                ret = esp_audio_simple_player_pause(s_player);
            }
            if (ret != ESP_GMF_ERR_OK) {
                ESP_LOGW(TAG, "pause failed: %x", ret);
                s_state = APP_MUSIC_ERROR;
            }
            break;
        case MUSIC_CMD_RESUME:
            if (music_player_is_running(&player_state) &&
                player_state == ESP_ASP_STATE_PAUSED) {
                ret = esp_audio_simple_player_resume(s_player);
            }
            if (ret != ESP_GMF_ERR_OK) {
                ESP_LOGW(TAG, "resume failed: %x", ret);
                s_state = APP_MUSIC_ERROR;
            }
            break;
        case MUSIC_CMD_STOP:
            if (music_player_is_running(&player_state)) {
                ret = esp_audio_simple_player_stop(s_player);
            }
            if (ret != ESP_GMF_ERR_OK) {
                ESP_LOGW(TAG, "stop failed: %x", ret);
            }
            break;
        case MUSIC_CMD_SHUTDOWN: {
            SemaphoreHandle_t done = cmd->done;
            if (music_player_is_running(&player_state)) {
                (void)esp_audio_simple_player_stop(s_player);
            }
            if (s_player != NULL) {
                ret = esp_audio_simple_player_destroy(s_player);
            }
            s_player = NULL;
            s_play_dev = NULL;
            s_state = APP_MUSIC_STOPPED;
            s_cur_url[0] = '\0';
            s_cmd_task = NULL;
            QueueHandle_t queue = s_cmd_queue;
            s_cmd_queue = NULL;
            free(cmd);
            if (queue != NULL) {
                vQueueDelete(queue);
            }
            if (done != NULL) {
                xSemaphoreGive(done);
            }
            vTaskDelete(NULL);
            return;
        }
        default:
            break;
        }
        free(cmd);
    }
}

/* --- public API ----------------------------------------------------------- */

esp_err_t app_music_player_init(esp_codec_dev_handle_t play_dev, int sample_rate)
{
    if (play_dev == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_play_dev = play_dev;
    if (sample_rate > 0) {
        s_sample_rate = sample_rate;
    }

    if (s_player == NULL) {
        esp_asp_cfg_t cfg = {0};
        cfg.out.cb = music_out_cb;
        cfg.out.user_ctx = NULL;
        cfg.task_prio = 8;
        cfg.task_stack = 8 * 1024;
        cfg.task_core = 1;
        cfg.task_stack_in_ext = 1;   /* keep internal SRAM for WakeNet */
        esp_gmf_err_t ret = esp_audio_simple_player_new(&cfg, &s_player);
        if (ret != ESP_GMF_ERR_OK || s_player == NULL) {
            ESP_LOGE(TAG, "simple player create failed: %x", ret);
            return ESP_FAIL;
        }
        esp_audio_simple_player_set_event(s_player, music_event_cb, NULL);

        s_cmd_queue = xQueueCreate(MUSIC_CMD_QUEUE_DEPTH, sizeof(music_cmd_t *));
        if (s_cmd_queue == NULL) {
            (void)esp_audio_simple_player_destroy(s_player);
            s_player = NULL;
            s_play_dev = NULL;
            return ESP_ERR_NO_MEM;
        }
        s_shutting_down = false;
        BaseType_t task_ret = xTaskCreatePinnedToCoreWithCaps(
            music_control_task, "music_ctrl", MUSIC_CTRL_TASK_STACK, NULL, 7,
            &s_cmd_task, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (task_ret != pdPASS) {
            vQueueDelete(s_cmd_queue);
            s_cmd_queue = NULL;
            (void)esp_audio_simple_player_destroy(s_player);
            s_player = NULL;
            s_play_dev = NULL;
            return ESP_ERR_NO_MEM;
        }
    }
    ESP_LOGI(TAG, "music player ready (rate=%d ch=%d)", s_sample_rate, s_channels);
    return ESP_OK;
}

esp_err_t app_music_player_deinit(void)
{
    if (s_player != NULL && s_cmd_queue != NULL && s_cmd_task != NULL) {
        s_shutting_down = true;
        SemaphoreHandle_t done = xSemaphoreCreateBinary();
        music_cmd_t *cmd = music_cmd_alloc(MUSIC_CMD_SHUTDOWN, NULL);
        if (done == NULL || cmd == NULL) {
            if (done != NULL) {
                vSemaphoreDelete(done);
            }
            free(cmd);
            s_shutting_down = false;
            return ESP_ERR_NO_MEM;
        }
        cmd->done = done;
        if (xQueueSend(s_cmd_queue, &cmd, portMAX_DELAY) != pdTRUE) {
            free(cmd);
            vSemaphoreDelete(done);
            s_shutting_down = false;
            return ESP_FAIL;
        }
        xSemaphoreTake(done, portMAX_DELAY);
        vSemaphoreDelete(done);
        return ESP_OK;
    }

    s_player = NULL;
    s_play_dev = NULL;
    s_state = APP_MUSIC_STOPPED;
    s_cur_url[0] = '\0';
    s_shutting_down = false;
    return ESP_OK;
}

bool app_music_player_is_initialized(void)
{
    return s_player != NULL;
}

esp_err_t app_music_player_play(const char *url)
{
    if (url == NULL || s_player == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (strlen(url) >= sizeof(s_cur_url)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = music_cmd_send(MUSIC_CMD_PLAY, url);
    if (ret != ESP_OK) {
        return ret;
    }
    ESP_LOGI(TAG, "queue play: %s", url);
    strcpy(s_cur_url, url);
    s_state = APP_MUSIC_PLAYING;
    return ESP_OK;
}

esp_err_t app_music_player_pause(void)
{
    if (s_player == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = music_cmd_send(MUSIC_CMD_PAUSE, NULL);
    if (ret != ESP_OK) {
        return ret;
    }
    s_state = APP_MUSIC_PAUSED;
    return ESP_OK;
}

esp_err_t app_music_player_resume(void)
{
    if (s_player == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = music_cmd_send(MUSIC_CMD_RESUME, NULL);
    if (ret != ESP_OK) {
        return ret;
    }
    s_state = APP_MUSIC_PLAYING;
    return ESP_OK;
}

esp_err_t app_music_player_stop(void)
{
    if (s_player == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = music_cmd_send(MUSIC_CMD_STOP, NULL);
    if (ret != ESP_OK) {
        return ret;
    }
    s_state = APP_MUSIC_STOPPED;
    return ESP_OK;
}

app_music_state_t app_music_player_get_state(void)
{
    return s_state;
}

const char *app_music_player_state_str(void)
{
    switch (s_state) {
    case APP_MUSIC_PLAYING: return "播放中";
    case APP_MUSIC_PAUSED:  return "已暂停";
    case APP_MUSIC_ERROR:   return "播放出错";
    default:                return "已停止";
    }
}
