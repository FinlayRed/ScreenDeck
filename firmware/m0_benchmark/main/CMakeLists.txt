idf_component_register(
    SRCS "m0_benchmark.c"
    EMBED_FILES "m0_frame.jpg"
    INCLUDE_DIRS "."
    REQUIRES esp32_p4_wifi6_touch_lcd_5 esp_driver_jpeg esp_psram esp_timer fatfs
)
