/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t boot_splash_start(void);
void boot_splash_wait(void);
void boot_splash_stop(void);

#ifdef __cplusplus
}
#endif
