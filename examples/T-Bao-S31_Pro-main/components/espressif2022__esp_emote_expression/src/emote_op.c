/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "expression_emote.h"
#include "esp_mmap_assets.h"

#include "emote_defs.h"
#include "emote_table.h"
#include "emote_layout.h"

// ===== Constants and Macros =====
static const char *TAG = "Expression_op";

#define HIDE_OBJ(handle, obj_type) do { \
    gfx_obj_t *obj = (handle)->def_objects[obj_type].obj; \
    if (obj) gfx_obj_set_visible(obj, false); \
} while(0)

#define SHOW_OBJ(handle, obj_type) do { \
    gfx_obj_t *obj = (handle)->def_objects[obj_type].obj; \
    if (obj) gfx_obj_set_visible(obj, true); \
} while(0)

#define EVENT_TABLE_SIZE (sizeof(event_table) / sizeof(event_table[0]))

/* Speech bubbles use the bundled 14px Puhui glyph advances.  Its line box is
 * 17px, so keep a little extra vertical room for the baseline and AA pixels.
 * The display resolution is still read from the emote handle so the layout
 * works on the 320x320 EAF panel and other configured resolutions. */
#define EMOTE_SPEECH_BUBBLE_FONT_PX          14
#define EMOTE_SPEECH_BUBBLE_LINE_HEIGHT      18
#define EMOTE_SPEECH_BUBBLE_LINE_SPACING     2
#define EMOTE_SPEECH_BUBBLE_PAD_X            12
#define EMOTE_SPEECH_BUBBLE_PAD_Y            8
#define EMOTE_SPEECH_BUBBLE_TEXT_SAFETY_X    4
#define EMOTE_SPEECH_BUBBLE_SCREEN_MARGIN    10
#define EMOTE_SPEECH_BUBBLE_SHORT_MAX_CHARS  8
#define EMOTE_SPEECH_BUBBLE_MEDIUM_MAX_CHARS 20
#define EMOTE_SPEECH_BUBBLE_MIN_WIDTH        92
#define EMOTE_SPEECH_BUBBLE_MIN_HEIGHT       48
#define EMOTE_SPEECH_BUBBLE_ARROW_DEPTH      10

#define EMOTE_SPEECH_BUBBLE_MAX(a, b) ((a) > (b) ? (a) : (b))
#define EMOTE_SPEECH_BUBBLE_MIN(a, b) ((a) < (b) ? (a) : (b))

// ===== Type Definitions =====
typedef esp_err_t (*emote_event_handler_t)(emote_handle_t handle, const char *message);

typedef struct {
    const char *event_name;
    emote_event_handler_t handler;
    bool skip_hide_ui;
} emote_event_entry_t;

// ===== Static Function Declarations =====
// Helper functions
static void **emote_get_cache_ptr_by_obj_type(emote_handle_t handle, emote_obj_type_t obj_type);
static gfx_image_dsc_t *emote_get_img_dsc_by_obj_type(emote_handle_t handle, emote_obj_type_t obj_type);
static void emote_set_eye_hidden(emote_handle_t handle, bool hidden);
static void emote_clear_speech_bubble(emote_handle_t handle);

// UI helper functions
static esp_err_t emote_set_icon_image(emote_handle_t handle, emote_obj_type_t obj_type, const char *name, bool visible);
static esp_err_t emote_set_icon_animation(emote_handle_t handle, emote_obj_type_t obj_type,
        const char *name, uint8_t fps, bool loop);
static esp_err_t emote_set_label_text(emote_handle_t handle, emote_obj_type_t obj_type, const char *text);
static esp_err_t emote_set_emoji_animation(emote_handle_t handle, emote_obj_type_t obj_type, const char *name);
static size_t emote_speech_utf8_count(const char *text);
static int emote_speech_char_width(const char **cursor);
static void emote_speech_measure(const char *text, int max_width, int *line_count, int *max_line_width);
static int emote_speech_bubble_height(int line_count, int arrow_depth);
static int emote_speech_arrow_depth(int width, int height);

// Event handler functions
static esp_err_t emote_handle_idle_event(emote_handle_t handle, const char *message);
static esp_err_t emote_handle_listen_event(emote_handle_t handle, const char *message);
static esp_err_t emote_handle_speak_event(emote_handle_t handle, const char *message);
static esp_err_t emote_handle_sys_set_event(emote_handle_t handle, const char *message);
static esp_err_t emote_handle_off_event(emote_handle_t handle, const char *message);
static esp_err_t emote_handle_bat_event(emote_handle_t handle, const char *message);

// Timer callback
static void emote_dialog_timer_cb(void *data);

// ===== Static Variables =====
static const emote_event_entry_t event_table[] = {
    { EMOTE_MGR_EVT_IDLE,   emote_handle_idle_event,    false },
    { EMOTE_MGR_EVT_LISTEN, emote_handle_listen_event,  false },
    { EMOTE_MGR_EVT_SPEAK,  emote_handle_speak_event,   false },
    { EMOTE_MGR_EVT_SYS,    emote_handle_sys_set_event, false },
    { EMOTE_MGR_EVT_SET,    emote_handle_sys_set_event, false },
    { EMOTE_MGR_EVT_BAT,    emote_handle_bat_event,     true  },
    { EMOTE_MGR_EVT_OFF,    emote_handle_off_event,     false  },
};

static bool s_speech_bubble_right;

// ===== Static Function Implementations =====

static size_t emote_speech_utf8_count(const char *text)
{
    size_t count = 0;
    const char *cursor = text;

    if (!text) {
        return 0;
    }

    while (*cursor) {
        const char *before = cursor;
        int width = emote_speech_char_width(&cursor);
        if (cursor == before) {
            break;
        }
        if (width != -1) {
            count++;
        }
    }
    return count;
}

static int emote_speech_char_width(const char **cursor)
{
    const unsigned char *p;
    int bytes = 1;

    if (!cursor || !*cursor || **cursor == '\0') {
        return 0;
    }

    p = (const unsigned char *)*cursor;
    if (*p == '\n') {
        (*cursor)++;
        return -1;
    }
    if (*p < 0x80) {
        (*cursor)++;
        if (*p == '\t') {
            return EMOTE_SPEECH_BUBBLE_FONT_PX;
        }
        return *p == ' ' ? EMOTE_SPEECH_BUBBLE_FONT_PX / 2 : EMOTE_SPEECH_BUBBLE_FONT_PX / 2 + 1;
    }
    if ((*p & 0xE0) == 0xC0) {
        bytes = 2;
    } else if ((*p & 0xF0) == 0xE0) {
        bytes = 3;
    } else if ((*p & 0xF8) == 0xF0) {
        bytes = 4;
    }

    for (int i = 1; i < bytes; i++) {
        if (p[i] == '\0' || (p[i] & 0xC0) != 0x80) {
            bytes = 1;
            break;
        }
    }
    *cursor += bytes;
    return EMOTE_SPEECH_BUBBLE_FONT_PX;
}

static void emote_speech_measure(const char *text, int max_width, int *line_count, int *max_line_width)
{
    const char *cursor = text;
    int lines = 1;
    int line_width = 0;
    int widest = 0;

    if (max_width < EMOTE_SPEECH_BUBBLE_FONT_PX) {
        max_width = EMOTE_SPEECH_BUBBLE_FONT_PX;
    }

    while (cursor && *cursor) {
        int char_width = emote_speech_char_width(&cursor);
        if (char_width == -1) {
            widest = EMOTE_SPEECH_BUBBLE_MAX(widest, line_width);
            line_width = 0;
            lines++;
            continue;
        }
        if (char_width <= 0) {
            continue;
        }
        if (line_width > 0 && line_width + char_width > max_width) {
            widest = EMOTE_SPEECH_BUBBLE_MAX(widest, line_width);
            line_width = char_width;
            lines++;
        } else {
            line_width += char_width;
        }
    }

    widest = EMOTE_SPEECH_BUBBLE_MAX(widest, line_width);
    if (line_count) {
        *line_count = EMOTE_SPEECH_BUBBLE_MAX(lines, 1);
    }
    if (max_line_width) {
        *max_line_width = EMOTE_SPEECH_BUBBLE_MAX(widest, EMOTE_SPEECH_BUBBLE_FONT_PX);
    }
}

static int emote_speech_bubble_height(int line_count, int arrow_depth)
{
    line_count = EMOTE_SPEECH_BUBBLE_MAX(line_count, 1);
    arrow_depth = EMOTE_SPEECH_BUBBLE_MAX(arrow_depth, 0);
    return line_count * EMOTE_SPEECH_BUBBLE_LINE_HEIGHT +
           (line_count - 1) * EMOTE_SPEECH_BUBBLE_LINE_SPACING +
           EMOTE_SPEECH_BUBBLE_PAD_Y * 2 + arrow_depth;
}

static int emote_speech_arrow_depth(int width, int height)
{
    int min_dim = EMOTE_SPEECH_BUBBLE_MIN(width, height);
    if (min_dim < 8) {
        return 0;
    }
    return EMOTE_SPEECH_BUBBLE_MIN(EMOTE_SPEECH_BUBBLE_ARROW_DEPTH,
                                   EMOTE_SPEECH_BUBBLE_MAX(4, min_dim / 4));
}

// Helper functions
static void **emote_get_cache_ptr_by_obj_type(emote_handle_t handle, emote_obj_type_t obj_type)
{
    if (!handle) {
        return NULL;
    }

    switch (obj_type) {
    case EMOTE_DEF_OBJ_ANIM_LISTEN:
    case EMOTE_DEF_OBJ_ANIM_EYE:
    case EMOTE_DEF_OBJ_ANIM_EMERG_DLG:
        if (!handle->def_objects[obj_type].data.anim) {
            handle->def_objects[obj_type].data.anim = (emote_anim_data_t *)calloc(1, sizeof(emote_anim_data_t));
            if (!handle->def_objects[obj_type].data.anim) {
                return NULL;
            }
            handle->def_objects[obj_type].data.anim->type = obj_type;
        }
        return (void **)&handle->def_objects[obj_type].data.anim->cache;
    case EMOTE_DEF_OBJ_ICON_STATUS:
    case EMOTE_DEF_OBJ_ICON_CHARGE:
        if (!handle->def_objects[obj_type].data.img) {
            handle->def_objects[obj_type].data.img = (emote_image_data_t *)calloc(1, sizeof(emote_image_data_t));
            if (!handle->def_objects[obj_type].data.img) {
                return NULL;
            }
            handle->def_objects[obj_type].data.img->type = obj_type;
        }
        return &handle->def_objects[obj_type].data.img->cache;
    default:
        return NULL;
    }
}

static gfx_image_dsc_t *emote_get_img_dsc_by_obj_type(emote_handle_t handle, emote_obj_type_t obj_type)
{
    if (!handle) {
        return NULL;
    }

    switch (obj_type) {
    case EMOTE_DEF_OBJ_ICON_STATUS:
    case EMOTE_DEF_OBJ_ICON_CHARGE:
        // Ensure image_cache is allocated (should be done by emote_get_cache_ptr_by_obj_type first)
        if (handle->def_objects[obj_type].data.img) {
            return &handle->def_objects[obj_type].data.img->img_dsc;
        }
        return NULL;
    default:
        return NULL;
    }
}

static void emote_set_eye_hidden(emote_handle_t handle, bool hidden)
{
    if (!handle) {
        return;
    }

    gfx_emote_lock(handle->gfx_handle);
    if (hidden) {
        HIDE_OBJ(handle, EMOTE_DEF_OBJ_ANIM_EYE);
    } else {
        SHOW_OBJ(handle, EMOTE_DEF_OBJ_ANIM_EYE);
    }
    gfx_emote_unlock(handle->gfx_handle);
}

static void emote_clear_speech_bubble(emote_handle_t handle)
{
    if (!handle) {
        return;
    }

    gfx_obj_t *obj = handle->def_objects[EMOTE_DEF_OBJ_LABEL_TOAST].obj;
    if (obj) {
        gfx_label_set_text(obj, "");
        gfx_obj_set_visible(obj, false);
    }
}

// UI helper functions
static esp_err_t emote_set_icon_image(emote_handle_t handle, emote_obj_type_t obj_type, const char *name, bool visible)
{
    esp_err_t ret = ESP_OK;
    icon_data_t *icon = NULL;
    gfx_obj_t *obj = NULL;
    void **cache_ptr = NULL;
    gfx_image_dsc_t *img_dsc = NULL;
    const void *src_data = NULL;

    ESP_GOTO_ON_FALSE(handle && name, ESP_ERR_INVALID_ARG, error, TAG, "Invalid parameters");

    ret = emote_get_icon_data_by_name(handle, name, &icon);
    if (ret != ESP_OK) {
        /* Icon asset absent from the flashed pack; the animated face still
         * renders fine without it, so skip silently instead of erroring. */
        return ESP_OK;
    }

    obj = handle->def_objects[obj_type].obj;
    ESP_GOTO_ON_FALSE(obj, ESP_ERR_INVALID_STATE, error, TAG, "object not found");

    ESP_GOTO_ON_FALSE(icon->data, ESP_ERR_INVALID_STATE, error, TAG, "icon.data is null");

    cache_ptr = emote_get_cache_ptr_by_obj_type(handle, obj_type);
    ESP_GOTO_ON_FALSE(cache_ptr, ESP_ERR_INVALID_STATE, error, TAG, "Failed to get cache pointer for object type %d", obj_type);

    img_dsc = emote_get_img_dsc_by_obj_type(handle, obj_type);
    ESP_GOTO_ON_FALSE(img_dsc, ESP_ERR_INVALID_STATE, error, TAG, "Failed to get image descriptor for object type %d", obj_type);

    gfx_emote_lock(handle->gfx_handle);
    src_data = emote_acquire_data(handle, icon->data, icon->size, cache_ptr);
    ESP_GOTO_ON_FALSE(src_data, ESP_ERR_INVALID_STATE, error_unlock, TAG, "Failed to acquire icon data");

    memcpy(&img_dsc->header, src_data, sizeof(gfx_image_header_t));
    img_dsc->data = (const uint8_t *)src_data + sizeof(gfx_image_header_t);
    img_dsc->data_size = icon->size - sizeof(gfx_image_header_t);

    gfx_img_set_src(obj, img_dsc);
    gfx_obj_set_visible(obj, visible);
    gfx_emote_unlock(handle->gfx_handle);
    return ESP_OK;

error_unlock:
    gfx_emote_unlock(handle->gfx_handle);

error:
    return ret;
}

static esp_err_t emote_set_icon_animation(emote_handle_t handle, emote_obj_type_t obj_type,
        const char *name, uint8_t fps, bool loop)
{
    esp_err_t ret = ESP_OK;
    icon_data_t *icon = NULL;
    gfx_obj_t *obj = NULL;
    void **cache_ptr = NULL;
    const void *src_data = NULL;

    ESP_GOTO_ON_FALSE(handle && name, ESP_ERR_INVALID_ARG, error, TAG, "Invalid parameters");

    ret = emote_get_icon_data_by_name(handle, name, &icon);
    if (ret != ESP_OK) {
        /* Optional listen artwork may be absent from a reduced asset pack. */
        return ESP_OK;
    }

    obj = handle->def_objects[obj_type].obj;
    ESP_GOTO_ON_FALSE(obj, ESP_ERR_INVALID_STATE, error, TAG, "Object not found");

    cache_ptr = emote_get_cache_ptr_by_obj_type(handle, obj_type);
    ESP_GOTO_ON_FALSE(cache_ptr, ESP_ERR_INVALID_STATE, error, TAG,
                     "Failed to get cache pointer for object type %d", obj_type);

    gfx_emote_lock(handle->gfx_handle);
    src_data = emote_acquire_data(handle, icon->data, icon->size, cache_ptr);
    ESP_GOTO_ON_FALSE(src_data, ESP_ERR_INVALID_STATE, error_unlock, TAG,
                      "Failed to acquire animation data");

    gfx_anim_set_src(obj, src_data, icon->size);
    gfx_anim_set_segment(obj, 0, 0xFFFF, fps, loop);
    gfx_anim_start(obj);
    gfx_obj_set_visible(obj, true);
    gfx_emote_unlock(handle->gfx_handle);
    return ESP_OK;

error_unlock:
    gfx_emote_unlock(handle->gfx_handle);

error:
    return ret;
}

static esp_err_t emote_set_label_text(emote_handle_t handle, emote_obj_type_t obj_type,
                                      const char *text)
{
    esp_err_t ret = ESP_OK;
    gfx_obj_t *obj = NULL;

    ESP_GOTO_ON_FALSE(handle, ESP_ERR_INVALID_ARG, error, TAG, "Invalid parameters");

    obj = handle->def_objects[obj_type].obj;
    if (!obj && !(obj = handle->def_objects[EMOTE_DEF_OBJ_LEBAL_DEFAULT].obj)) {
        ret = ESP_ERR_INVALID_STATE;
        goto error;
    }

    gfx_emote_lock(handle->gfx_handle);
    gfx_label_set_text(obj, text ? text : "");
    gfx_obj_set_visible(obj, true);
    gfx_emote_unlock(handle->gfx_handle);
    return ESP_OK;

error:
    return ret;
}

static esp_err_t emote_set_emoji_animation(emote_handle_t handle, emote_obj_type_t obj_type,
        const char *name)
{
    esp_err_t ret = ESP_OK;
    emoji_data_t *emoji = NULL;
    gfx_obj_t *obj = NULL;
    void **cache_ptr = NULL;
    const void *src_data = NULL;

    ESP_GOTO_ON_FALSE(handle && name, ESP_ERR_INVALID_ARG, error, TAG, "Invalid parameters");

    ret = emote_get_emoji_data_by_name(handle, name, &emoji);
    ESP_GOTO_ON_FALSE(ret == ESP_OK, ret, error, TAG, "Not found:\"%s\"", name);

    ESP_LOGD(TAG, "Setting emoji: %s (fps=%d, loop=%s)",
             name, emoji->fps, emoji->loop ? "true" : "false");

    obj = handle->def_objects[obj_type].obj;
    ESP_GOTO_ON_FALSE(obj, ESP_ERR_INVALID_STATE, error, TAG, "%s object not found", name);

    cache_ptr = emote_get_cache_ptr_by_obj_type(handle, obj_type);
    ESP_GOTO_ON_FALSE(cache_ptr, ESP_ERR_INVALID_STATE, error, TAG, "Failed to get cache pointer for object type %d", obj_type);

    gfx_emote_lock(handle->gfx_handle);
    src_data = emote_acquire_data(handle, emoji->data, emoji->size, cache_ptr);
    ESP_GOTO_ON_FALSE(src_data, ESP_ERR_INVALID_STATE, error_unlock, TAG, "Failed to acquire %s animation data", name);

    gfx_anim_set_src(obj, src_data, emoji->size);
    gfx_anim_set_segment(obj, 0, 0xFFFF, emoji->fps > 0 ? emoji->fps : EMOTE_DEF_ANIMATION_FPS, emoji->loop);
    gfx_anim_start(obj);
    gfx_obj_set_visible(obj, true);

    gfx_emote_unlock(handle->gfx_handle);
    return ESP_OK;

error_unlock:
    gfx_emote_unlock(handle->gfx_handle);

error:
    return ret;
}

// Event handler functions
static esp_err_t emote_handle_idle_event(emote_handle_t handle, const char *message)
{
    (void)message;
    esp_err_t ret = ESP_OK;

    ret = emote_set_bat_status(handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set battery status");
    }

    ret = emote_set_label_clock(handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set clock label");
    }

    return ESP_OK;
}

static esp_err_t emote_handle_listen_event(emote_handle_t handle, const char *message)
{
    esp_err_t ret = ESP_OK;

    (void)message;
    emote_clear_speech_bubble(handle);

    ret = emote_set_icon_animation(handle, EMOTE_DEF_OBJ_ANIM_LISTEN, EMOTE_ICON_LISTEN, 15, true);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set listen animation");
    }

    ret = emote_set_icon_image(handle, EMOTE_DEF_OBJ_ICON_STATUS, EMOTE_ICON_MIC, true);
    return ret;
}

static esp_err_t emote_handle_speak_event(emote_handle_t handle, const char *message)
{
    esp_err_t ret = ESP_OK;
    gfx_obj_t *obj = NULL;
    size_t char_count;
    int screen_w;
    int screen_h;
    int max_width;
    int line_count = 1;
    int max_line_width = EMOTE_SPEECH_BUBBLE_FONT_PX;
    int bubble_width;
    int bubble_height;
    int arrow_depth;
    int text_offset_y;
    uint8_t align = GFX_ALIGN_TOP_MID;
    gfx_label_bg_shape_t shape = GFX_LABEL_BG_SHAPE_ROUND_RECT;
    gfx_label_bg_arrow_t arrow = GFX_LABEL_BG_ARROW_BOTTOM_CENTER;

    ESP_GOTO_ON_FALSE(handle, ESP_ERR_INVALID_ARG, error, TAG, "Invalid parameters");

    obj = handle->def_objects[EMOTE_DEF_OBJ_LABEL_TOAST].obj;
    ESP_GOTO_ON_FALSE(obj, ESP_ERR_INVALID_STATE, error, TAG, "Toast label not found");

    if (!message || message[0] == '\0') {
        emote_clear_speech_bubble(handle);
        return ESP_OK;
    }

    screen_w = handle->h_res > 0 ? handle->h_res : 320;
    screen_h = handle->v_res > 0 ? handle->v_res : 320;
    max_width = EMOTE_SPEECH_BUBBLE_MAX(EMOTE_SPEECH_BUBBLE_FONT_PX,
                                        screen_w - EMOTE_SPEECH_BUBBLE_SCREEN_MARGIN * 2 -
                                        EMOTE_SPEECH_BUBBLE_PAD_X * 2 -
                                        EMOTE_SPEECH_BUBBLE_TEXT_SAFETY_X);
    char_count = emote_speech_utf8_count(message);

    ret = gfx_label_set_text(obj, message);
    ESP_GOTO_ON_ERROR(ret, error, TAG, "Failed to set speech bubble text");

    /* Short messages use a playful blob in the upper corners.  Alternating
     * sides keeps repeated replies from covering the same face detail. */
    if (char_count <= EMOTE_SPEECH_BUBBLE_SHORT_MAX_CHARS) {
        emote_speech_measure(message, max_width, &line_count, &max_line_width);
        bubble_width = EMOTE_SPEECH_BUBBLE_MAX(EMOTE_SPEECH_BUBBLE_MIN_WIDTH,
                                               max_line_width + EMOTE_SPEECH_BUBBLE_PAD_X * 2 +
                                               EMOTE_SPEECH_BUBBLE_TEXT_SAFETY_X);
        bubble_width = EMOTE_SPEECH_BUBBLE_MIN(bubble_width,
                                               screen_w - EMOTE_SPEECH_BUBBLE_SCREEN_MARGIN * 2);
        bubble_height = EMOTE_SPEECH_BUBBLE_MAX(
            EMOTE_SPEECH_BUBBLE_MIN_HEIGHT,
            emote_speech_bubble_height(line_count, EMOTE_SPEECH_BUBBLE_ARROW_DEPTH));
        shape = GFX_LABEL_BG_SHAPE_BLOB;
        arrow = s_speech_bubble_right ? GFX_LABEL_BG_ARROW_BOTTOM_LEFT : GFX_LABEL_BG_ARROW_BOTTOM_RIGHT;
        align = s_speech_bubble_right ? GFX_ALIGN_TOP_RIGHT : GFX_ALIGN_TOP_LEFT;
        s_speech_bubble_right = !s_speech_bubble_right;
    } else if (char_count <= EMOTE_SPEECH_BUBBLE_MEDIUM_MAX_CHARS) {
        emote_speech_measure(message, max_width, &line_count, &max_line_width);
        bubble_width = EMOTE_SPEECH_BUBBLE_MAX(EMOTE_SPEECH_BUBBLE_MIN_WIDTH + 20,
                                               max_line_width + EMOTE_SPEECH_BUBBLE_PAD_X * 2 +
                                               EMOTE_SPEECH_BUBBLE_TEXT_SAFETY_X);
        bubble_width = EMOTE_SPEECH_BUBBLE_MIN(bubble_width,
                                               screen_w - EMOTE_SPEECH_BUBBLE_SCREEN_MARGIN * 2);
        emote_speech_measure(message,
                             EMOTE_SPEECH_BUBBLE_MAX(EMOTE_SPEECH_BUBBLE_FONT_PX,
                                                     bubble_width - EMOTE_SPEECH_BUBBLE_PAD_X * 2 -
                                                     EMOTE_SPEECH_BUBBLE_TEXT_SAFETY_X),
                             &line_count, &max_line_width);
        bubble_height = EMOTE_SPEECH_BUBBLE_MAX(
            EMOTE_SPEECH_BUBBLE_MIN_HEIGHT,
            emote_speech_bubble_height(line_count, EMOTE_SPEECH_BUBBLE_ARROW_DEPTH));
        shape = GFX_LABEL_BG_SHAPE_PILL;
        arrow = GFX_LABEL_BG_ARROW_BOTTOM_CENTER;
        align = GFX_ALIGN_TOP_MID;
    } else {
        bubble_width = EMOTE_SPEECH_BUBBLE_MAX(EMOTE_SPEECH_BUBBLE_FONT_PX + 2,
                                               screen_w - EMOTE_SPEECH_BUBBLE_SCREEN_MARGIN * 2);
        bubble_width = EMOTE_SPEECH_BUBBLE_MIN(bubble_width, screen_w);
        emote_speech_measure(message,
                             EMOTE_SPEECH_BUBBLE_MAX(EMOTE_SPEECH_BUBBLE_FONT_PX,
                                                     bubble_width - EMOTE_SPEECH_BUBBLE_PAD_X * 2 -
                                                     EMOTE_SPEECH_BUBBLE_TEXT_SAFETY_X),
                             &line_count, &max_line_width);
        bubble_height = emote_speech_bubble_height(line_count,
                                                   EMOTE_SPEECH_BUBBLE_ARROW_DEPTH);
        bubble_height = EMOTE_SPEECH_BUBBLE_MIN(bubble_height,
                                                EMOTE_SPEECH_BUBBLE_MAX(EMOTE_SPEECH_BUBBLE_MIN_HEIGHT,
                                                                        screen_h - EMOTE_SPEECH_BUBBLE_SCREEN_MARGIN * 2));
        shape = GFX_LABEL_BG_SHAPE_ROUND_RECT;
        arrow = GFX_LABEL_BG_ARROW_TOP_CENTER;
        align = GFX_ALIGN_BOTTOM_MID;
    }

    arrow_depth = emote_speech_arrow_depth(bubble_width, bubble_height);
    if (arrow_depth > 0) {
        int base_height = emote_speech_bubble_height(line_count, 0);
        if (arrow == GFX_LABEL_BG_ARROW_TOP_CENTER) {
            bubble_height = EMOTE_SPEECH_BUBBLE_MIN(
                EMOTE_SPEECH_BUBBLE_MAX(1,
                                        screen_h - EMOTE_SPEECH_BUBBLE_SCREEN_MARGIN * 2),
                base_height + arrow_depth);
        }
    }

    gfx_obj_set_size(obj, (uint16_t)bubble_width, (uint16_t)bubble_height);
    if (align == GFX_ALIGN_TOP_LEFT) {
        gfx_obj_align(obj, GFX_ALIGN_TOP_LEFT, EMOTE_SPEECH_BUBBLE_SCREEN_MARGIN,
                      EMOTE_SPEECH_BUBBLE_SCREEN_MARGIN);
    } else if (align == GFX_ALIGN_TOP_RIGHT) {
        gfx_obj_align(obj, GFX_ALIGN_TOP_RIGHT, -EMOTE_SPEECH_BUBBLE_SCREEN_MARGIN,
                      EMOTE_SPEECH_BUBBLE_SCREEN_MARGIN);
    } else if (align == GFX_ALIGN_BOTTOM_MID) {
        gfx_obj_align(obj, GFX_ALIGN_BOTTOM_MID, 0, -4);
    } else {
        gfx_obj_align(obj, GFX_ALIGN_TOP_MID, 0, 16);
    }

    gfx_label_set_color(obj, GFX_COLOR_HEX(0x000000u));
    gfx_label_set_bg_color(obj, GFX_COLOR_HEX(0xFFFFFFu));
    gfx_label_set_opa(obj, 0xFF);
    gfx_label_set_bg_enable(obj, true);
    gfx_label_set_bg_shape(obj, shape);
    gfx_label_set_bg_arrow(obj, arrow);
    text_offset_y = EMOTE_SPEECH_BUBBLE_PAD_Y +
                    (arrow == GFX_LABEL_BG_ARROW_TOP_CENTER ? arrow_depth : 0);
    gfx_label_set_text_offset(obj, EMOTE_SPEECH_BUBBLE_PAD_X, text_offset_y);
    gfx_label_set_line_spacing(obj, EMOTE_SPEECH_BUBBLE_LINE_SPACING);
    gfx_label_set_text_align(obj, GFX_TEXT_ALIGN_CENTER);
    gfx_label_set_long_mode(obj, GFX_LABEL_LONG_WRAP);
    gfx_obj_set_visible(obj, true);
    ret = emote_set_icon_image(handle, EMOTE_DEF_OBJ_ICON_STATUS, EMOTE_ICON_SPEAKER, true);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set speaker icon");
    }
    return ret;

error:
    return ret;
}

static esp_err_t emote_handle_sys_set_event(emote_handle_t handle, const char *message)
{
    esp_err_t ret = ESP_OK;

    ret = emote_set_label_text(handle, EMOTE_DEF_OBJ_LABEL_TOAST, message);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set label text");
    }

    ret = emote_set_icon_image(handle, EMOTE_DEF_OBJ_ICON_STATUS, EMOTE_ICON_TIPS, true);

    gfx_obj_t *obj = handle->def_objects[EMOTE_DEF_OBJ_LABEL_TOAST].obj;
    if (obj) {
        /* System notices keep the original plain scrolling label style. */
        gfx_label_set_bg_enable(obj, false);
        gfx_label_set_snap_loop(obj, true);
    }

    return ret;
}

static esp_err_t emote_handle_off_event(emote_handle_t handle, const char *message)
{
    (void)message;
    emote_clear_speech_bubble(handle);
    // emote_set_eye_hidden(handle, true);
    return ESP_OK;
}

static esp_err_t emote_handle_bat_event(emote_handle_t handle, const char *message)
{
    esp_err_t ret = ESP_OK;

    ESP_GOTO_ON_FALSE(message, ESP_ERR_INVALID_ARG, error, TAG, "Message is NULL");

    // message format: "charging,percent" e.g. "1,75" or "0,30"
    char *comma_pos = strchr(message, ',');
    if (comma_pos) {
        int percent = atoi(comma_pos + 1);
        handle->bat_is_charging = (message[0] == '1');
        handle->bat_percent = (percent < 0) ? 0 : (percent > 100 ? 100 : percent);
    }
    return ESP_OK;

error:
    return ret;
}

// Timer callback
static void emote_dialog_timer_cb(void *data)
{
    emote_handle_t handle = (emote_handle_t)data;
    if (!handle) {
        return;
    }

    emote_stop_anim_dialog(handle);
}

// ===== Public Function Implementations =====

esp_err_t emote_set_bat_status(emote_handle_t handle)
{
    esp_err_t ret = ESP_OK;

    ESP_GOTO_ON_FALSE(handle, ESP_ERR_INVALID_ARG, error, TAG, "Invalid parameters");

    if (handle->bat_percent >= 0) {
        ret = emote_set_bat_status_label(handle);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to set battery label");
        }

        ret = emote_set_icon_image(handle, EMOTE_DEF_OBJ_ICON_STATUS, EMOTE_ICON_BATTERY_BG, true);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to set battery background icon");
        }

        ret = emote_set_bat_charge_icon(handle);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to set battery charge icon");
        }
    }
    return ESP_OK;

error:
    return ret;
}

esp_err_t emote_set_bat_charge_icon(emote_handle_t handle)
{
    esp_err_t ret = ESP_OK;

    ESP_GOTO_ON_FALSE(handle, ESP_ERR_INVALID_ARG, error, TAG, "Invalid parameters");

    if (handle->bat_percent < 0) {
        return ESP_OK;
    }

    ret = emote_set_icon_image(handle, EMOTE_DEF_OBJ_ICON_CHARGE, EMOTE_ICON_BATTERY_CHARGE, handle->bat_is_charging);

error:
    return ret;
}

void emote_set_bat_status_charge(emote_handle_t handle)
{
    if (!handle || handle->bat_percent < 0) {
        return;
    }

    gfx_obj_t *obj = handle->def_objects[EMOTE_DEF_OBJ_ICON_CHARGE].obj;
    if (!obj) {
        return;
    }

    gfx_emote_lock(handle->gfx_handle);
    gfx_obj_set_visible(obj, handle->bat_is_charging);
    gfx_emote_unlock(handle->gfx_handle);
}

esp_err_t emote_set_bat_status_label(emote_handle_t handle)
{
    esp_err_t ret = ESP_OK;

    ESP_GOTO_ON_FALSE(handle, ESP_ERR_INVALID_ARG, error, TAG, "Invalid parameters");

    if (handle->bat_percent >= 0) {
        char percent_str[16];
        snprintf(percent_str, sizeof(percent_str), "%d", handle->bat_percent);
        ret = emote_set_label_text(handle, EMOTE_DEF_OBJ_LABEL_BATTERY, percent_str);
    }

error:
    return ret;
}

esp_err_t emote_set_label_clock(emote_handle_t handle)
{
    esp_err_t ret = ESP_OK;
    gfx_obj_t *obj = NULL;
    gfx_timer_handle_t timer = NULL;

    ESP_GOTO_ON_FALSE(handle, ESP_ERR_INVALID_ARG, error, TAG, "Invalid parameters");

    obj = handle->def_objects[EMOTE_DEF_OBJ_LABEL_CLOCK].obj;
    ESP_GOTO_ON_FALSE(obj, ESP_ERR_INVALID_STATE, error, TAG, "CLOCK_LABEL object not found");

    timer = (gfx_timer_handle_t)handle->def_objects[EMOTE_DEF_OBJ_TIMER_STATUS].obj;
    ESP_GOTO_ON_FALSE(timer, ESP_ERR_INVALID_STATE, error, TAG, "CLOCK_TIMER object not found");

    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    char time_str[10];
    snprintf(time_str, sizeof(time_str), "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);

    gfx_emote_lock(handle->gfx_handle);
    gfx_label_set_text(obj, time_str);
    gfx_obj_set_visible(obj, true);

    if (!gfx_timer_is_running(timer)) {
        gfx_timer_resume(timer);
    }
    gfx_emote_unlock(handle->gfx_handle);
    return ESP_OK;

error:
    return ret;
}

esp_err_t emote_set_anim_emoji(emote_handle_t handle, const char *name)
{
    if (!handle || !name) {
        return ESP_ERR_INVALID_ARG;
    }

    emote_set_eye_hidden(handle, false);
    return emote_set_emoji_animation(handle, EMOTE_DEF_OBJ_ANIM_EYE, name);
}

esp_err_t emote_set_dialog_anim(emote_handle_t handle, const char *name)
{
    if (!handle || !name) {
        return ESP_ERR_INVALID_ARG;
    }

    emote_set_eye_hidden(handle, true);
    return emote_set_emoji_animation(handle, EMOTE_DEF_OBJ_ANIM_EMERG_DLG, name);
}

esp_err_t emote_set_qrcode_data(emote_handle_t handle, const char *qrcode_text)
{
    esp_err_t ret = ESP_OK;
    gfx_obj_t *obj = NULL;

    ESP_LOGI(TAG, "set_qrcode_data: %s", qrcode_text);
    ESP_GOTO_ON_FALSE(handle && qrcode_text, ESP_ERR_INVALID_ARG, error, TAG, "Invalid parameters");

    obj = handle->def_objects[EMOTE_DEF_OBJ_QRCODE].obj;
    ESP_GOTO_ON_FALSE(obj, ESP_ERR_INVALID_STATE, error, TAG, "QRCODE object not found");

    gfx_emote_lock(handle->gfx_handle);
    gfx_qrcode_set_data(obj, qrcode_text);
    gfx_obj_set_visible(obj, true);
    gfx_emote_unlock(handle->gfx_handle);
    return ESP_OK;

error:
    return ret;
}

esp_err_t emote_stop_anim_dialog(emote_handle_t handle)
{
    esp_err_t ret = ESP_OK;

    ESP_GOTO_ON_FALSE(handle, ESP_ERR_INVALID_ARG, error, TAG, "Invalid parameters");

    gfx_emote_lock(handle->gfx_handle);

    // Stop and delete timer if exists
    if (handle->dialog_timer) {
        gfx_timer_delete(handle->gfx_handle, handle->dialog_timer);
        handle->dialog_timer = NULL;
    }

    SHOW_OBJ(handle, EMOTE_DEF_OBJ_ANIM_EYE);
    HIDE_OBJ(handle, EMOTE_DEF_OBJ_ANIM_EMERG_DLG);

    emote_def_obj_entry_t *entry = &handle->def_objects[EMOTE_DEF_OBJ_ANIM_EMERG_DLG];
    if (entry->data.anim) {
        if (entry->data.anim->cache) {
            free(entry->data.anim->cache);
        }
        free(entry->data.anim);
        entry->data.anim = NULL;
    }
    gfx_emote_unlock(handle->gfx_handle);
    return ESP_OK;

error:
    return ret;
}

esp_err_t emote_insert_anim_dialog(emote_handle_t handle, const char *name, uint32_t duration_ms)
{
    esp_err_t ret = ESP_OK;
    gfx_timer_handle_t timer = NULL;

    ESP_GOTO_ON_FALSE(handle && name, ESP_ERR_INVALID_ARG, error, TAG, "Invalid parameters");

    // Reset semaphore before starting new animation
    if (handle->emerg_dlg_done_sem) {
        xSemaphoreTake(handle->emerg_dlg_done_sem, 0); // Clear semaphore if already set
    }

    gfx_emote_lock(handle->gfx_handle);
    if (handle->dialog_timer) {
        gfx_timer_delete(handle->gfx_handle, handle->dialog_timer);
        handle->dialog_timer = NULL;
    }
    gfx_emote_unlock(handle->gfx_handle);

    ret = emote_set_dialog_anim(handle, name);
    ESP_GOTO_ON_FALSE(ret == ESP_OK, ret, error, TAG, "Failed to set dialog animation");

    gfx_emote_lock(handle->gfx_handle);

    timer = gfx_timer_create(handle->gfx_handle, emote_dialog_timer_cb, duration_ms, handle);
    ESP_GOTO_ON_FALSE(timer, ESP_ERR_NO_MEM, error_unlock, TAG, "Failed to create dialog timer");

    gfx_timer_set_repeat_count(timer, 1);  // Execute only once
    handle->dialog_timer = timer;
    gfx_emote_unlock(handle->gfx_handle);

    return ESP_OK;

error_unlock:
    gfx_emote_unlock(handle->gfx_handle);
    emote_stop_anim_dialog(handle);

error:
    return ret;
}

esp_err_t emote_set_obj_visible(emote_handle_t handle, const char *name, bool visible)
{
    esp_err_t ret = ESP_OK;
    gfx_obj_t *obj = NULL;

    ESP_GOTO_ON_FALSE(handle && name, ESP_ERR_INVALID_ARG, error, TAG, "Invalid parameters");

    obj = emote_get_obj_by_name(handle, name);
    ESP_GOTO_ON_FALSE(obj, ESP_ERR_INVALID_STATE, error, TAG, "Object not found");

    gfx_emote_lock(handle->gfx_handle);
    gfx_obj_set_visible(obj, visible);
    gfx_emote_unlock(handle->gfx_handle);
    return ESP_OK;

error:
    return ret;
}

esp_err_t emote_set_anim_visible(emote_handle_t handle, bool visible)
{
    emote_set_eye_hidden(handle, !visible);
    return ESP_OK;
}

esp_err_t emote_set_event_msg(emote_handle_t handle, const char *event, const char *message)
{
    esp_err_t ret = ESP_OK;
    const emote_event_entry_t *entry = NULL;

    ESP_GOTO_ON_FALSE(handle && event, ESP_ERR_INVALID_ARG, error, TAG, "Invalid parameters");

    ESP_LOGD(TAG, "setEventMsg: %s, message: \"%s\"", event, message ? message : "");

    gfx_emote_lock(handle->gfx_handle);

    // Look up event handler in table
    for (size_t i = 0; i < EVENT_TABLE_SIZE; i++) {
        if (strcmp(event, event_table[i].event_name) == 0) {
            entry = &event_table[i];
            break;
        }
    }

    ESP_GOTO_ON_FALSE(entry, ESP_ERR_NOT_FOUND, error_unlock, TAG, "Unhandled event: %s", event);

    // Hide all UI elements for events that don't skip hiding
    if (!entry->skip_hide_ui) {
        HIDE_OBJ(handle, EMOTE_DEF_OBJ_ANIM_LISTEN);
        HIDE_OBJ(handle, EMOTE_DEF_OBJ_LABEL_CLOCK);
        HIDE_OBJ(handle, EMOTE_DEF_OBJ_LABEL_TOAST);
        HIDE_OBJ(handle, EMOTE_DEF_OBJ_LABEL_BATTERY);
        HIDE_OBJ(handle, EMOTE_DEF_OBJ_ICON_CHARGE);
        HIDE_OBJ(handle, EMOTE_DEF_OBJ_ICON_STATUS);
        HIDE_OBJ(handle, EMOTE_DEF_OBJ_QRCODE);
        HIDE_OBJ(handle, EMOTE_DEF_OBJ_LEBAL_DEFAULT);
        gfx_timer_handle_t obj_timer = (gfx_timer_handle_t)handle->def_objects[EMOTE_DEF_OBJ_TIMER_STATUS].obj;
        if (obj_timer) {
            gfx_timer_pause(obj_timer);
        }
    }

    // Call event handler
    ret = entry->handler(handle, message);

    gfx_emote_unlock(handle->gfx_handle);
    return ret;

error_unlock:
    gfx_emote_unlock(handle->gfx_handle);

error:
    return ret;
}

esp_err_t emote_notify_flush_finished(emote_handle_t handle)
{
    esp_err_t ret = ESP_OK;

    /* This callback fires as soon as the display has been added to the gfx
     * core, which happens before emote_init() finishes creating the default
     * label.  Gating on is_initialized here rejects the first boot-time flush,
     * leaving gfx_render blocked on WAIT_FLUSH_DONE forever.  The display
     * handle is the only thing this notification actually needs. */
    ESP_GOTO_ON_FALSE(handle, ESP_ERR_INVALID_STATE, error, TAG, "Handle is NULL");

    ESP_GOTO_ON_FALSE(handle->gfx_disp, ESP_ERR_INVALID_STATE, error, TAG, "GFX display handle not initialized");

    gfx_disp_flush_ready(handle->gfx_disp, true);
    return ESP_OK;

error:
    return ret;
}

esp_err_t emote_notify_all_refresh(emote_handle_t handle)
{
    esp_err_t ret = ESP_OK;

    ESP_GOTO_ON_FALSE(handle && handle->is_initialized, ESP_ERR_INVALID_STATE, error, TAG, "Handle not initialized");

    ESP_GOTO_ON_FALSE(handle->gfx_disp, ESP_ERR_INVALID_STATE, error, TAG, "GFX handle not initialized");

    gfx_disp_refresh_all(handle->gfx_disp);
    return ESP_OK;

error:
    return ret;
}


esp_err_t emote_lock(emote_handle_t handle)
{
    esp_err_t ret = ESP_OK;

    ESP_GOTO_ON_FALSE(handle && handle->is_initialized, ESP_ERR_INVALID_STATE, error, TAG, "Handle not initialized");

    ESP_GOTO_ON_FALSE(handle->gfx_handle, ESP_ERR_INVALID_STATE, error, TAG, "GFX handle not initialized");

    gfx_emote_lock(handle->gfx_handle);
    return ESP_OK;

error:
    return ret;
}


esp_err_t emote_unlock(emote_handle_t handle)
{
    esp_err_t ret = ESP_OK;

    ESP_GOTO_ON_FALSE(handle && handle->is_initialized, ESP_ERR_INVALID_STATE, error, TAG, "Handle not initialized");

    ESP_GOTO_ON_FALSE(handle->gfx_handle, ESP_ERR_INVALID_STATE, error, TAG, "GFX handle not initialized");

    gfx_emote_unlock(handle->gfx_handle);
    return ESP_OK;

error:
    return ret;
}
