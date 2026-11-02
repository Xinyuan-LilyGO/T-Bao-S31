/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ui_page_style.h"

#include "font_awesome_symbols.h"
#include "app_language.h"

LV_FONT_DECLARE(font_awesome_20_4);
LV_FONT_DECLARE(font_puhui_16_4);
LV_FONT_DECLARE(font_puhui_20_4);

#define UI_PAGE_BACKGROUND 0x000000
#define UI_PAGE_PANEL      0x151E24
#define UI_PAGE_LINE       0x2A3942
#define UI_PAGE_TEXT       0xF4F7F8
#define UI_PAGE_MUTED      0x8C9AA3

static void style_container(lv_obj_t *obj)
{
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN);
}

void ui_page_style_root(lv_obj_t *root)
{
    if (root == NULL) {
        return;
    }

    lv_obj_set_style_bg_color(root, lv_color_hex(UI_PAGE_BACKGROUND), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_radius(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_set_layout(root, LV_LAYOUT_NONE);
}

static lv_obj_t *make_header(lv_obj_t *root,
                             const char *icon,
                             const lv_image_dsc_t *image,
                             const char *title,
                             const char *subtitle,
                             uint32_t accent)
{
    if (root == NULL) {
        return NULL;
    }

    lv_obj_t *header = lv_obj_create(root);
    if (header == NULL) {
        return NULL;
    }
    lv_obj_set_size(header, LV_HOR_RES - 58, 48);
    lv_obj_set_pos(header, 50, 4);
    style_container(header);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(header, 8, 0);
    lv_obj_add_flag(header, LV_OBJ_FLAG_IGNORE_LAYOUT);

    if (image != NULL) {
        lv_obj_t *icon_image = lv_image_create(header);
        if (icon_image == NULL) {
            return header;
        }
        /* Reuse the launcher artwork, including its colors and alpha. */
        lv_image_set_src(icon_image, image);
        lv_obj_set_size(icon_image, 30, 30);
        lv_image_set_inner_align(icon_image, LV_IMAGE_ALIGN_CONTAIN);
    } else {
        lv_obj_t *icon_box = lv_obj_create(header);
        if (icon_box == NULL) {
            return header;
        }
        lv_obj_set_size(icon_box, 30, 30);
        lv_obj_set_style_bg_color(icon_box, lv_color_hex(accent), 0);
        lv_obj_set_style_radius(icon_box, 9, 0);
        style_container(icon_box);

        lv_obj_t *icon_label = lv_label_create(icon_box);
        if (icon_label != NULL) {
            lv_label_set_text(icon_label, icon != NULL ? icon : FONT_AWESOME_HOME);
            lv_obj_set_style_text_font(icon_label, &font_awesome_20_4, 0);
            lv_obj_set_style_text_color(icon_label, lv_color_hex(UI_PAGE_BACKGROUND), 0);
            lv_obj_center(icon_label);
        }
    }

    lv_obj_t *copy = lv_obj_create(header);
    if (copy == NULL) {
        return header;
    }
    lv_obj_set_size(copy, LV_HOR_RES - 112, 46);
    style_container(copy);
    lv_obj_set_style_bg_opa(copy, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(copy, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(copy, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(copy, 0, 0);

    lv_obj_t *title_label = lv_label_create(copy);
    if (title_label != NULL) {
        lv_label_set_text(title_label, app_language_text(title, NULL));
        lv_obj_set_width(title_label, LV_HOR_RES - 112);
        lv_obj_set_height(title_label, 24);
        lv_label_set_long_mode(title_label, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(title_label, &font_puhui_20_4, 0);
        lv_obj_set_style_text_color(title_label, lv_color_hex(UI_PAGE_TEXT), 0);
    }

    if (subtitle != NULL && subtitle[0] != '\0') {
        lv_obj_t *subtitle_label = lv_label_create(copy);
        if (subtitle_label != NULL) {
            lv_label_set_text(subtitle_label, app_language_text(subtitle, NULL));
            lv_obj_set_width(subtitle_label, LV_HOR_RES - 112);
            lv_obj_set_height(subtitle_label, 20);
            lv_label_set_long_mode(subtitle_label, LV_LABEL_LONG_DOT);
            lv_obj_set_style_text_font(subtitle_label, &font_puhui_16_4, 0);
            lv_obj_set_style_text_color(subtitle_label, lv_color_hex(UI_PAGE_MUTED), 0);
        }
    }

    return header;
}

lv_obj_t *ui_page_make_header(lv_obj_t *root,
                               const char *icon,
                               const char *title,
                               const char *subtitle,
                               uint32_t accent)
{
    return make_header(root, icon, NULL, title, subtitle, accent);
}

lv_obj_t *ui_page_make_header_image(lv_obj_t *root,
                                    const lv_image_dsc_t *image,
                                    const char *title,
                                    const char *subtitle)
{
    return make_header(root, NULL, image, title, subtitle, 0);
}

void ui_page_style_panel(lv_obj_t *obj)
{
    if (obj == NULL) {
        return;
    }

    lv_obj_set_style_bg_color(obj, lv_color_hex(UI_PAGE_PANEL), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, 12, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(UI_PAGE_LINE), 0);
    lv_obj_set_style_border_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
}

void ui_page_style_button(lv_obj_t *obj,
                          uint32_t color,
                          uint32_t pressed_color,
                          uint16_t radius)
{
    if (obj == NULL) {
        return;
    }

    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(pressed_color), LV_STATE_PRESSED);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(UI_PAGE_LINE), 0);
    lv_obj_set_style_border_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
}
