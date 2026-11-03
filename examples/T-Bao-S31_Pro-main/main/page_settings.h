/*
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "ui_page_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const ui_page_t page_settings;

/* Open the battery detail view when settings is already active. */
void page_settings_open_battery(void);

#ifdef __cplusplus
}
#endif
