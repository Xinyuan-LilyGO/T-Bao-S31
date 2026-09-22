idf_component_register(
    SRCS
        main.cpp
        audio_test.cpp
        display_ui.cpp
    INCLUDE_DIRS .
    PRIV_REQUIRES
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_i2s
        esp_lcd
        esp_timer
)

