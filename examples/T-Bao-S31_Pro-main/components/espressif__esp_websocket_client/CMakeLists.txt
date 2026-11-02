idf_build_get_property(target IDF_TARGET)

if(NOT CONFIG_WS_TRANSPORT AND NOT CMAKE_BUILD_EARLY_EXPANSION)
    message(STATUS "Websocket transport is disabled")
    idf_component_register()
    return()
endif()

if("${target}" STREQUAL "linux")
    idf_component_register(
        SRCS "esp_websocket_client_psram.c"
        INCLUDE_DIRS "include"
        REQUIRES esp_common esp-tls tcp_transport http_parser esp_event
        PRIV_REQUIRES esp_timer freertos heap)
else()
    idf_component_register(
        SRCS "esp_websocket_client_psram.c"
        INCLUDE_DIRS "include"
        REQUIRES esp_common lwip esp-tls tcp_transport http_parser esp_event
        PRIV_REQUIRES esp_timer freertos heap)
endif()
