idf_component_register(
    SRCS "m1_hid_touch.c"
    INCLUDE_DIRS "."
    REQUIRES esp32_p4_wifi6_touch_lcd_5 esp_timer esp_tinyusb
)
