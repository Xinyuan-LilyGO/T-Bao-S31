set(srcs
    "esp_lcd_st7796.c"
    "esp_lcd_st7796_general.c"
)

if(CONFIG_SOC_MIPI_DSI_SUPPORTED)
    list(APPEND srcs "esp_lcd_st7796_mipi.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "priv_include"
    REQUIRES esp_lcd
    PRIV_REQUIRES driver
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    ESP_LCD_ST7796_VER_MAJOR=1
    ESP_LCD_ST7796_VER_MINOR=4
    ESP_LCD_ST7796_VER_PATCH=0
)
