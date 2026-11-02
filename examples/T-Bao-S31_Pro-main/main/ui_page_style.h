/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shared visual primitives for the 320x320 app pages.
 */
#pragma once

#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

void ui_page_style_root(lv_obj_t *root);

lv_obj_t *ui_page_make_header(lv_obj_t *root,
                               const char *icon,
                               const char *title,
                               const char *subtitle,
                               uint32_t accent);

lv_obj_t *ui_page_make_header_image(lv_obj_t *root,
                                    const lv_image_dsc_t *image,
                                    const char *title,
                                    const char *subtitle);

void ui_page_style_panel(lv_obj_t *obj);

void ui_page_style_button(lv_obj_t *obj,
                          uint32_t color,
                          uint32_t pressed_color,
                          uint16_t radius);

#ifdef __cplusplus
}
#endif
