// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts M5's media scheduler after the shared M3 display has been created. */
void m5_media_start(lv_display_t *display);
uint32_t m5_media_trigger_screensaver(void);
void m5_hid_release_all(const char *reason);

#ifdef __cplusplus
}
#endif
