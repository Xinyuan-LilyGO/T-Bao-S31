idf_component_register(
    SRCS
        main.cpp
        display_ui.cpp
        wifi_test.cpp
    INCLUDE_DIRS .
    PRIV_REQUIRES
        esp_driver_gpio
        esp_event
        esp_lcd
        esp_netif
        esp_timer
        esp_wifi
        nvs_flash
)
