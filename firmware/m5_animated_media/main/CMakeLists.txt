idf_component_register(
    SRCS
        "m5_media.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../m3_sync_storage/main/m3_sync_storage.c"
    INCLUDE_DIRS "."
    REQUIRES esp32_p4_wifi6_touch_lcd_5 esp_timer esp_tinyusb fatfs esp_driver_jpeg esp_lv_decoder)

target_compile_definitions(${COMPONENT_LIB} PRIVATE M5_MEDIA_ENABLED=1)
