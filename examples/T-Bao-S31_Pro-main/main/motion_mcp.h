#pragma once

#include "esp_err.h"
#include "esp_mcp_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register the vehicle and head controls on the XiaoZhi MCP engine. */
esp_err_t motion_mcp_register_tools(esp_mcp_t *mcp);

#ifdef __cplusplus
}
#endif
