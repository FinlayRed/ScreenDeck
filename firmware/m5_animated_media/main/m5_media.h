#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts M5's media scheduler after the shared M3 display has been created. */
void m5_media_start(lv_display_t *display);

#ifdef __cplusplus
}
#endif
