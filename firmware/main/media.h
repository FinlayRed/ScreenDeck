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
#define PANEL_WIDTH 720
#define PANEL_HEIGHT 1280
#define MAX_FRAMES 1800
#define MAX_JPEG_BYTES (2U * 1024U * 1024U)
#define ICON_FPS 15
#define ICON_MAX_FRAMES 120

/* Starts the media scheduler after the display has been created. */
void media_start(lv_display_t *display);
uint32_t media_trigger_screensaver(void);
void hid_release_all(const char *reason);

/* Side-effect-free structural validation of a ScreenDeck bundle payload.
 * `payload_offset` is the payload's byte offset from the file start and
 * `payload_size` bounds every read. The validator seeks to the payload before
 * reading it. Checks
 * the UI bundle magic, schema, table ranges, counts, references, assets, animation
 * streams, and typed-table alignment (F3/F4). Never allocates the payload and
 * never mutates media state. */
bool ui_bundle_valid(FILE *file, long payload_offset, uint32_t payload_size);

/* Side-effect-free validation of a complete MJPEG screensaver stream on disk:
 * complete SOI/EOI frame boundaries, frame count within MAX_FRAMES, per-
 * frame size within MAX_JPEG_BYTES, and every frame decoding to
 * PANEL_WIDTH x PANEL_HEIGHT (F7). Used before media activation and for
 * boot recovery. */
bool mjpeg_file_valid(const char *path);

/* Media control requests. The media task is the sole owner of screensaver
 * file handles and buffers, so cross-task work (bundle/media sync, screensaver
 * testing) must be serialized through media_control() instead of mutating
 * media state directly. */
typedef enum {
    /* Close screensaver handles and buffers and stop playback. Used by the
     * sync task before renaming the active screensaver file. */
    MEDIA_CTRL_QUIESCE = 1,
    /* Close any handles, then re-index the screensaver file. Used after a
     * media commit so the pre-restart window keeps a valid playback state. */
    MEDIA_CTRL_RELOAD = 2,
    /* Index the screensaver if needed, then request playback. Used by the
     * test-screensaver USB opcode; serializes against any in-progress
     * indexing so only one index pass can run at a time. */
    MEDIA_CTRL_TEST = 3,
} media_ctrl_t;

/* Sends a control request and waits up to timeout_ms for the media task to
 * acknowledge it. Returns 0 on success, the media indexer diagnostic on a
 * failed index, or UINT32_MAX if the request could not be queued or
 * acknowledged. Must not be called from the media task itself. */
uint32_t media_control(media_ctrl_t control, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
