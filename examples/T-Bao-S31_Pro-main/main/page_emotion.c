/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "page_emotion.h"

#include "app_sfx.h"
#include "app_language.h"
#include "esp_xiaozhi_chat_display.h"
#include "font_awesome_symbols.h"
#include "lvgl.h"
#include "page_launcher.h"
#include "ui_page_style.h"
#include "launcher_icons.h"

LV_FONT_DECLARE(font_puhui_16_4);
LV_FONT_DECLARE(font_puhui_20_4);

typedef struct {
    const char *name;
    const char *label;
} emotion_item_t;

static const emotion_item_t s_emotions[] = {
    { "neutral",     "自然" },
    { "happy",       "开心" },
    { "sad",         "难过" },
    { "crying",      "哭泣" },
    { "sleepy",      "困倦" },
    { "angry",       "生气" },
    { "shocked",     "震惊" },
    { "confused",    "困惑" },
    { "winking",     "眨眼" },
};
#define EMOTION_COUNT (sizeof(s_emotions) / sizeof(s_emotions[0]))
#define EMOTION_COLS 3
#define EMOTION_ROWS 3
#define EMOTION_GRID_PAD 6
#define EMOTION_GRID_BORDER 1
#define EMOTION_GRID_COLUMN_GAP 5
#define EMOTION_GRID_ROW_GAP 8
#define EMOTION_BUTTON_HEIGHT 62

static const emotion_item_t *s_selected = &s_emotions[0];
static lv_obj_t *s_selected_label;

static void emotion_button_cb(lv_event_t *event)
{
    const emotion_item_t *item = (const emotion_item_t *)lv_event_get_user_data(event);
    if (item == NULL) {
        return;
    }

    app_sfx_play(APP_SFX_TAP);
    s_selected = item;
    esp_xiaozhi_chat_display_set_emotion(item->name);
    ui_page_manager_switch(PAGE_FACE);
}

static void add_emotion_button(lv_obj_t *grid, const emotion_item_t *item,
                               int width)
{
    lv_obj_t *button = lv_btn_create(grid);
    lv_obj_set_size(button, width, EMOTION_BUTTON_HEIGHT);
    ui_page_style_button(button, 0x151E24, 0x2A3942, 12);
    lv_obj_set_style_border_color(button, lv_color_hex(0xBE9CFF), 0);
    lv_obj_set_style_pad_all(button, 4, 0);
    lv_obj_set_flex_flow(button, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(button, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(button, emotion_button_cb, LV_EVENT_CLICKED,
                        (void *)item);

    lv_obj_t *name = lv_label_create(button);
    lv_label_set_text(name, item->name);
    lv_obj_set_style_text_font(name, &font_puhui_16_4, 0);
    lv_obj_set_style_text_color(name, lv_color_hex(0x3D8BFD), 0);

    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, app_language_text(item->label, NULL));
    lv_obj_set_style_text_font(label, &font_puhui_20_4, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
}

esp_err_t page_emotion_on_enter(void)
{
    lv_obj_t *root = esp_xiaozhi_chat_display_page_root();
    if (root == NULL) {
        return ESP_FAIL;
    }

    ui_page_style_root(root);
    ui_page_make_back_button(root);
    ui_page_make_header_image(root, &launcher_icon_emotion, "表情", "动画选择");

    lv_obj_t *grid = lv_obj_create(root);
    lv_obj_set_size(grid, LV_HOR_RES - 20, LV_VER_RES - 104);
    lv_obj_align(grid, LV_ALIGN_TOP_MID, 0, 52);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    ui_page_style_panel(grid);
    lv_obj_set_style_pad_all(grid, EMOTION_GRID_PAD, 0);
    lv_obj_set_style_pad_row(grid, EMOTION_GRID_ROW_GAP, 0);
    lv_obj_set_style_pad_column(grid, EMOTION_GRID_COLUMN_GAP, 0);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_set_scroll_dir(grid, LV_DIR_NONE);
    lv_obj_set_scrollbar_mode(grid, LV_SCROLLBAR_MODE_OFF);

    const int grid_width = LV_HOR_RES - 20;
    const int grid_content_width = grid_width -
                                   (2 * EMOTION_GRID_PAD) -
                                   (2 * EMOTION_GRID_BORDER);
    const int button_width = (grid_content_width -
                              (EMOTION_COLS - 1) * EMOTION_GRID_COLUMN_GAP) /
                             EMOTION_COLS;
    for (size_t i = 0; i < EMOTION_COUNT; ++i) {
        add_emotion_button(grid, &s_emotions[i], button_width);
    }

    s_selected_label = lv_label_create(root);
    lv_obj_set_width(s_selected_label, LV_HOR_RES - 16);
    lv_obj_set_style_text_font(s_selected_label, &font_puhui_16_4, 0);
    lv_obj_set_style_text_align(s_selected_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_selected_label, lv_color_hex(0xA0A0A8), 0);
    lv_label_set_text_fmt(s_selected_label,
                          app_language_is_english() ? "Current animation  %s" :
                                                      "当前动画  %s",
                          app_language_text(s_selected->label, NULL));
    lv_obj_align(s_selected_label, LV_ALIGN_BOTTOM_MID, 0, -9);

    app_sfx_play(APP_SFX_PAGE);
    return ESP_OK;
}

esp_err_t page_emotion_on_exit(void)
{
    s_selected_label = NULL;
    return ESP_OK;
}

const ui_page_t page_emotion = {
    .id = PAGE_EMOTION,
    .name = "emotion",
    .on_enter = page_emotion_on_enter,
    .on_exit = page_emotion_on_exit,
};
