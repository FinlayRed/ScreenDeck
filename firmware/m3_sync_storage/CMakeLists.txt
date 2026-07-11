cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../../vendor/waveshare-reference/examples/esp-idf/10_mp4_player/components/esp32_p4_wifi6_touch_lcd_5"
    "${CMAKE_CURRENT_LIST_DIR}/../../vendor/waveshare-reference/examples/esp-idf/10_mp4_player/components/esp_lcd_hx8394"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(screendeck_m3_sync_storage)
