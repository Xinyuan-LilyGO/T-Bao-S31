/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "page_about.h"

#include "esp_xiaozhi_chat_display.h"
#include "app_language.h"
#include "font_awesome_symbols.h"
#include "lvgl.h"
#include "page_launcher.h"
#include "ui_page_style.h"
#include "launcher_icons.h"

LV_FONT_DECLARE(font_awesome_30_4);
LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_16_4);

esp_err_t page_about_on_enter(void)
{
    lv_obj_t *root = esp_xiaozhi_chat_display_page_root();
    if (root == NULL) {
        return ESP_FAIL;
    }

    ui_page_style_root(root);
    ui_page_make_back_button(root);
    ui_page_make_header_image(root, &launcher_icon_about, "关于设备", "喵伴 S31");

    lv_obj_t *card = lv_obj_create(root);
    lv_obj_set_size(card, LV_HOR_RES - 24, 218);
    lv_obj_set_pos(card, 12, 58);
    ui_page_style_panel(card);
    lv_obj_set_style_pad_all(card, 12, 0);

    lv_obj_t *name = lv_label_create(card);
    lv_label_set_text(name, app_language_text("ESP32-S3 交互宠物", "ESP32-S3 interactive pet"));
    lv_obj_set_style_text_font(name, &font_puhui_20_4, 0);
    lv_obj_set_style_text_color(name, lv_color_white(), 0);
    lv_obj_align(name, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *info = lv_label_create(card);
    lv_obj_set_width(info, LV_HOR_RES - 48);
    lv_obj_set_height(info, 140);
    lv_obj_align(info, LV_ALIGN_TOP_LEFT, 0, 46);
    lv_label_set_long_mode(info, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(info, &font_puhui_16_4, 0);
    lv_obj_set_style_text_align(info, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_line_space(info, 8, 0);
    lv_obj_set_style_text_color(info, lv_color_hex(0xD0D0D8), 0);
    lv_label_set_text(info, app_language_is_english() ?
                      "Display  ST7796S / 320 x 320\n"
                      "Touch    FT6336\n"
                      "Camera   OV2640 / OV3660\n"
                      "Audio    ES8389 / ES7210\n"
                      "Apps     Chat · Music · Video · Games" :
                      "屏幕     ST7796S / 320 x 320\n"
                      "触摸     FT6336\n"
                      "摄像头   OV2640 / OV3660\n"
                      "音频     ES8389 / ES7210\n"
                      "应用     对话 · 音乐 · 视频 · 游戏");

    lv_obj_t *version = lv_label_create(root);
    lv_label_set_text(version, app_language_text("固件版本  2026.10", "Firmware  2026.10"));
    lv_obj_set_style_text_font(version, &font_puhui_16_4, 0);
    lv_obj_set_style_text_color(version, lv_color_hex(0x8C9AA3), 0);
    lv_obj_align(version, LV_ALIGN_BOTTOM_MID, 0, -16);

    return ESP_OK;
}

esp_err_t page_about_on_exit(void)
{
    return ESP_OK;
}

const ui_page_t page_about = {
    .id = PAGE_ABOUT,
    .name = "about",
    .on_enter = page_about_on_enter,
    .on_exit = page_about_on_exit,
};
