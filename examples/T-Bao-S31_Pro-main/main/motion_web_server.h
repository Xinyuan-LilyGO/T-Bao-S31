#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t motion_web_server_start(void);
void motion_web_server_stop(void);

#ifdef __cplusplus
}
#endif
