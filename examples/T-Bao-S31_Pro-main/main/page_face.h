/*
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include "ui_page_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The animated face page (esp_emote_expression). Drives idle emotion rotation,
 * random tap changes, long-press laugh reactions, and forwards chat + status
 * to the emote toast/status events. */
extern const ui_page_t page_face;

#ifdef __cplusplus
}
#endif
