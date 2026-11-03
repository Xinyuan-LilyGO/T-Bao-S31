/*
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_LANGUAGE_ZH_CN = 0,
    APP_LANGUAGE_EN_US = 1,
} app_language_t;

esp_err_t app_language_init(void);
app_language_t app_language_get(void);
bool app_language_is_english(void);
esp_err_t app_language_set(app_language_t language);
esp_err_t app_language_toggle(void);
const char *app_language_name(void);
const char *app_language_code(void);

/* Return the selected translation. When en is NULL, a built-in common-label
 * dictionary is used so existing page literals can be migrated incrementally. */
const char *app_language_text(const char *zh, const char *en);

#ifdef __cplusplus
}
#endif
