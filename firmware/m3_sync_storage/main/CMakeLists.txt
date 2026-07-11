idf_component_register(
    SRCS "m3_sync_storage.c"
    INCLUDE_DIRS "."
    REQUIRES esp32_p4_wifi6_touch_lcd_5 esp_timer esp_tinyusb fatfs
)
