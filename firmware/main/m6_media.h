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

/* Media control requests. The media task is the sole owner of screensaver
 * file handles and buffers, so cross-task work (bundle/media sync, screensaver
 * testing) must be serialized through m5_media_control() instead of mutating
 * media state directly. */
typedef enum {
    /* Close screensaver handles and buffers and stop playback. Used by the
     * sync task before renaming the active screensaver file. */
    M5_MEDIA_CTRL_QUIESCE = 1,
    /* Close any handles, then re-index the screensaver file. Used after a
     * media commit so the pre-restart window keeps a valid playback state. */
    M5_MEDIA_CTRL_RELOAD = 2,
    /* Index the screensaver if needed, then request playback. Used by the
     * test-screensaver USB opcode; serializes against any in-progress
     * indexing so only one index pass can run at a time. */
    M5_MEDIA_CTRL_TEST = 3,
} m5_media_ctrl_t;

/* Sends a control request and waits up to timeout_ms for the media task to
 * acknowledge it. Returns 0 on success, the media indexer diagnostic on a
 * failed index, or UINT32_MAX if the request could not be queued or
 * acknowledged. Must not be called from the media task itself. */
uint32_t m5_media_control(m5_media_ctrl_t control, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
