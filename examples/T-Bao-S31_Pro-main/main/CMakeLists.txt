set(SOURCES "esp_xiaozhi_chat_app.c" "esp_xiaozhi_chat_display.c" "app_main.c" "wifi_provisioning.c" "app_audio_dsp.c"
            "app_chat_history.c" "app_sfx.c" "app_camera.c" "http_preview.c"             "app_music_player.c"
             "app_sdcard.c"
             "app_mp4_demux.c" "app_avi_demux.c" "app_video_overlay.c" "app_video_player.c"
             "app_recorder.c"
             "app_settings.c"
             "app_language.c"
             "app_camera_preview.c" "app_vision_lite.c" "app_guard_mode.c"
             "app_activity.c"
             "face_target_tracker.c" "face_follow_controller.c" "app_face_follow.cpp"
             "tbao_s31_board.c"
            "actuator_control.cpp"
            "motion_math.c" "motion_manager.c" "motion_behavior.c" "motion_mcp.c" "motion_web_server.cpp"
            "muyu_gif.c" "launcher_icons.c"
            "ui_touch.c" "ui_page_manager.c" "ui_control_center.c" "ui_page_style.c"
            "page_face.c" "page_launcher.c" "page_xiaozhi.c" "page_dialogue.c" "page_camera.c" "page_music.c" "page_video.c" "page_game.c" "page_timer.c" "page_motion.c" "page_face_follow.c"
             "page_emotion.c" "page_settings.c"
             "boot_splash.c"
             "battery_estimator.c" "battery_monitor.c")
set(INCLUDE_DIRS "." "../components/esp_xiaozhi/src/priv_include")

idf_component_register(SRCS ${SOURCES}
                       INCLUDE_DIRS ${INCLUDE_DIRS}
                       REQUIRES
                         protocol_examples_common
                         esp_board_manager_adapter
                         esp_xiaozhi
                         jason-mao__av_processor
                         espressif__esp_codec_dev
                         espressif__esp_audio_effects
                         espressif__esp_board_manager
                         espressif__esp_lcd_touch
                         haqqscripter__esp_cam_io_parl
                         esp_driver_jpeg
                         espressif__esp_io_expander
                         espressif__esp_lvgl_port
                         lvgl__lvgl
                         78__xiaozhi-fonts
                         espressif2022__esp_emote_expression
                         espressif2022__esp_emote_gfx
                         espressif__esp_mmap_assets
                         espressif__esp_audio_simple_player
                         esp-dl
                         human_face_detect
                         espressif__esp-sr
                         espressif__gmf_ai_audio
                         espressif__esp_video_codec
                         espressif__mcp-c-sdk
                         esp_http_server
                         esp_http_client
                         espressif__cjson
                         mbedtls
                         lwip
                         nvs_flash
                         esp_wifi
                         esp_event
                         esp_netif
                         fatfs
                         driver
                         esp_timer
                         esp_partition
                       PRIV_REQUIRES
                         esp_app_format
                       WHOLE_ARCHIVE
                      )

set(BOOT_SPLASH_GIF_FILE "${CMAKE_CURRENT_LIST_DIR}/../assets/welcome_boot.gif")
file(SIZE "${BOOT_SPLASH_GIF_FILE}" BOOT_SPLASH_GIF_SIZE)
target_compile_definitions(${COMPONENT_LIB} PRIVATE
                           BOOT_SPLASH_GIF_SIZE=${BOOT_SPLASH_GIF_SIZE})
esptool_py_flash_to_partition(flash "welcome" "${BOOT_SPLASH_GIF_FILE}")

set(EMOTE_ASSETS_FILE "${CMAKE_BINARY_DIR}/assets.bin")
build_speaker_assets_bin("assets" "360_360" "${EMOTE_ASSETS_FILE}" "${CONFIG_MMAP_FILE_NAME_LENGTH}")
esptool_py_flash_to_partition(flash "assets" "${EMOTE_ASSETS_FILE}")

idf_component_get_property(av_processor_lib jason-mao__av_processor COMPONENT_LIB)
idf_component_get_property(esp_sr_dir espressif__esp-sr COMPONENT_DIR)
idf_component_get_property(esp_partition_lib esp_partition COMPONENT_LIB)
idf_component_get_property(cjson_lib espressif__cjson COMPONENT_LIB)
idf_component_get_property(gmf_ai_audio_lib espressif__gmf_ai_audio COMPONENT_LIB)
target_include_directories(${av_processor_lib} PRIVATE
                           "${esp_sr_dir}/include/${IDF_TARGET}"
                           "${esp_sr_dir}/src/include")
target_link_libraries(${av_processor_lib} PRIVATE
                      ${esp_partition_lib}
                      ${cjson_lib}
                      ${gmf_ai_audio_lib})
