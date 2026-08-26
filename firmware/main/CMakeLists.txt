# SPDX-License-Identifier: GPL-3.0-or-later

idf_component_register(
    SRCS
        "media.c"
        "radial.c"
        "storage.c"
    INCLUDE_DIRS "."
    REQUIRES esp32_p4_wifi6_touch_lcd_5 esp_timer esp_tinyusb fatfs esp_driver_jpeg esp_lv_decoder esp_driver_ledc)

target_compile_definitions(${COMPONENT_LIB} PRIVATE MEDIA_ENABLED=1 RADIAL_UX_ENABLED=1)
