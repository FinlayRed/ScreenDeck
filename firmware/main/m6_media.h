// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Shared screensaver media contract (mirrored by the editor and the standalone
 * converter; see AUDIT_AND_FIX_PLAN.md Phase 3). */
#define M5_PANEL_WIDTH 720
#define M5_PANEL_HEIGHT 1280
#define M5_MAX_FRAMES 1800
#define M5_MAX_JPEG_BYTES (2U * 1024U * 1024U)
#define M5_ICON_FPS 15
#define M5_ICON_MAX_FRAMES 120

/* Starts M5's media scheduler after the shared M3 display has been created. */
void m5_media_start(lv_display_t *display);
uint32_t m5_media_trigger_screensaver(void);
void m5_hid_release_all(const char *reason);

/* Side-effect-free structural validation of an SDB3 payload (M5UI).
 * `payload_offset` is the payload's byte offset from the file start and
 * `payload_size` bounds every read. The validator seeks to the payload before
 * reading it. Checks
 * the M5UI magic, schema, table ranges, counts, references, assets, animation
 * streams, and typed-table alignment (F3/F4). Never allocates the payload and
 * never mutates media state. */
bool m5_ui_bundle_valid(FILE *file, long payload_offset, uint32_t payload_size);

/* Side-effect-free validation of a complete MJPEG screensaver stream on disk:
 * complete SOI/EOI frame boundaries, frame count within M5_MAX_FRAMES, per-
 * frame size within M5_MAX_JPEG_BYTES, and every frame decoding to
 * M5_PANEL_WIDTH x M5_PANEL_HEIGHT (F7). Used before media activation and for
 * boot recovery. */
bool m5_mjpeg_file_valid(const char *path);

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
