/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "sdkconfig.h"
#include "expression_emote.h"
#include "esp_mmap_assets.h"
#include "gfx.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

// Forward declaration
typedef struct assets_hash_table_s assets_hash_table_t;

// ===== DEFAULT VALUES =====
#define EMOTE_DEF_SCROLL_SPEED          CONFIG_EMOTE_DEF_SCROLL_SPEED
#define EMOTE_DEF_LABEL_HEIGHT          CONFIG_EMOTE_DEF_LABEL_HEIGHT
#define EMOTE_DEF_LABEL_WIDTH           CONFIG_EMOTE_DEF_LABEL_WIDTH
#define EMOTE_DEF_LABEL_Y_OFFSET        CONFIG_EMOTE_DEF_LABEL_Y_OFFSET
#define EMOTE_DEF_ANIMATION_FPS         CONFIG_EMOTE_DEF_ANIMATION_FPS
#define EMOTE_DEF_FONT_COLOR            CONFIG_EMOTE_DEF_FONT_COLOR
#define EMOTE_DEF_BG_COLOR              CONFIG_EMOTE_DEF_BG_COLOR

// ===== ICON NAME CONSTANTS =====
#define EMOTE_INDEX_JSON_FILENAME       "index.json"
#define EMOTE_ICON_MIC                  "icon_mic"
#define EMOTE_ICON_TIPS                 "icon_tips"
#define EMOTE_ICON_LISTEN               "listen"
#define EMOTE_ICON_SPEAKER              "icon_speaker"
#define EMOTE_ICON_BATTERY_BG           "battery_bg"
#define EMOTE_ICON_BATTERY_CHARGE       "battery_charge"

// ===== OBJECT TYPE ENUM =====
typedef enum {
    EMOTE_DEF_OBJ_LEBAL_DEFAULT = 0,  // For default label
    EMOTE_DEF_OBJ_ANIM_EYE,           // For AI buddy eye expressions
    EMOTE_DEF_OBJ_ANIM_LISTEN,        // For listening indicator
    EMOTE_DEF_OBJ_ANIM_EMERG_DLG,     // For emergency dialog
    EMOTE_DEF_OBJ_ICON_STATUS,        // For status indicators
    EMOTE_DEF_OBJ_LABEL_TOAST,        // For text notifications
    EMOTE_DEF_OBJ_LABEL_CLOCK,        // For time display
    EMOTE_DEF_OBJ_TIMER_STATUS,       // For status timer
    EMOTE_DEF_OBJ_LABEL_BATTERY,      // For battery percent display
    EMOTE_DEF_OBJ_ICON_CHARGE,        // For battery charge icon
    EMOTE_DEF_OBJ_QRCODE,             // For QR code display
    EMOTE_DEF_OBJ_MAX                 // Boundary marker
} emote_obj_type_t;

// ===== Type Definitions =====

typedef struct {
    emote_obj_type_t type;
    void *cache;
    gfx_image_dsc_t img_dsc;
} emote_image_data_t;

typedef struct {
    emote_obj_type_t type;
    void *cache;
} emote_anim_data_t;

/** User data union for different object types
 *  Automatically matches user data structure based on object type
 */
typedef union {
    emote_anim_data_t *anim;           // For animation objects (EMOTE_DEF_OBJ_ANIM_*)
    emote_image_data_t *img;           // For image objects (EMOTE_DEF_OBJ_ICON_*)
    // gfx_timer_handle_t timer;          // For timer objects (EMOTE_DEF_OBJ_TIMER_STATUS)
} emote_obj_data_t;

/** Default object entry with integrated user data
 *  Automatically associates user data based on object type using union
 */
typedef struct {
    gfx_obj_t *obj;                    // Object pointer
    emote_obj_data_t data;    // User data union, automatically matches by type
} emote_def_obj_entry_t;

typedef struct emote_custom_obj_entry_s {
    char *name;                    // Object name (dynamically allocated)
    gfx_obj_t *obj;                // Object pointer
    struct emote_custom_obj_entry_s *next;  // Next entry in linked list
} emote_custom_obj_entry_t;

struct emote_s {
    bool is_initialized;

    gfx_handle_t gfx_handle;
    gfx_disp_t *gfx_disp;
    mmap_assets_handle_t assets_handle;

    /** Default objects with integrated cache
     *  Cache is automatically associated based on object type
     */
    emote_def_obj_entry_t def_objects[EMOTE_DEF_OBJ_MAX];
    emote_custom_obj_entry_t *custom_objects;  // Linked list for custom objects

    //font cache
    lv_font_t *gfx_font;
    void *font_cache;

    //battery cache
    bool bat_is_charging;
    int8_t bat_percent;

    assets_hash_table_t *emoji_table;
    assets_hash_table_t *icon_table;

    //dialog timer [EMOTE_DEF_OBJ_ANIM_EMERG_DLG]
    gfx_timer_handle_t dialog_timer;

    //signal for emergency dialog animation completion
    SemaphoreHandle_t emerg_dlg_done_sem;

    //callback
    emote_flush_ready_cb_t flush_cb;
    emote_update_cb_t update_cb;

    //resolution
    int h_res;
    int v_res;
    void *user_data;
};

#ifdef __cplusplus
}
#endif
