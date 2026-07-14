/*
 * M6 — radial gestures, integrated settings, animated media and screensaver.
 *
 * The M3 transport remains the owner of the card and the USB device.  This
 * module only consumes a fixed, device-local media path after that transport
 * and the shared LVGL display are ready.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "bsp/esp32_p4_wifi6_touch_lcd_5.h"
#include "driver/jpeg_decode.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_log.h"
#include "esp_lv_decoder.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "class/hid/hid_device.h"
#include "lvgl.h"
#include "draw/lv_image_decoder_private.h"
#include "misc/cache/instance/lv_image_cache.h"
#include "m6_media.h"
#include "m6_radial.h"

static const char *TAG = "m6";

#define M5_COLUMNS 8
#define M5_ROWS 4
#define M5_BUTTONS (M5_COLUMNS * M5_ROWS)
#define M5_GAP_PX 8
#define M5_MARGIN_X_PX 16
#define M5_ACTIVE_POLL_MS 5
#define M5_DEFAULT_SCREENSAVER_IDLE_SECONDS 15
#define M5_SCREENSAVER_FPS 30
#define M5_MAX_FRAMES 900
#define M5_PRELOAD_LIMIT (8U * 1024U * 1024U)
#define M5_MAX_JPEG_BYTES (2U * 1024U * 1024U)
#define M5_JPEG_PATH BSP_SD_MOUNT_POINT "/screendeck/screensaver.mjpg"
#define M5_LCD_WIDTH 1280
#define M5_LCD_HEIGHT 720
#define M5_PANEL_WIDTH 720
#define M5_PANEL_HEIGHT 1280
#define M5_RGB565_BYTES (M5_LCD_WIDTH * M5_LCD_HEIGHT * 2U)
#define M5_INDEX_BUFFER_BYTES (16U * 1024U)
#define M5_UI_MAGIC 0x4955354DUL
#define M5_SDB3_MAGIC 0x33424453UL
#define M5_ICON_FPS 15
#define M5_ICON_MAX_FRAMES 120
#define M5_ICON_UPDATE_BATCH 4
#define M5_ICON_MEDIUM_LOAD_FPS 10
#define M5_ICON_HEAVY_LOAD_FPS 7
#define M5_MACRO_SLOTS 8
#define M5_HID_KEYBOARD_REPORT_ID 1
#define M5_HID_CONSUMER_REPORT_ID 2
#define M6_RADIAL_ACTION_PROFILE_NEXT (UINT16_MAX - 3)
#define M6_RADIAL_ACTION_PAGE_PREVIOUS (UINT16_MAX - 2)
#define M6_RADIAL_ACTION_PAGE_NEXT (UINT16_MAX - 1)
#define M6_RADIAL_ACTION_NONE UINT16_MAX
#define M6_RADIAL_OPEN_DRAG_PX 5
#define M6_RADIAL_PREWARM_MAX (M5_BUTTONS * 8)
#define M6_RADIAL_PREWARM_INITIAL_MS 30
#define M6_RADIAL_PREWARM_PERIOD_MS 1

typedef enum {
    M5_STATE_ACTIVE,
    M5_STATE_PLAYING,
    M5_STATE_WAKING,
} m5_state_t;

typedef struct {
    uint32_t offset;
    uint32_t length;
} m5_frame_index_t;

typedef struct {
    FILE *file;
    uint32_t file_size;
    uint32_t frame_count;
    uint32_t largest_frame;
    m5_frame_index_t frames[M5_MAX_FRAMES];
    uint8_t *preload;
    uint8_t *read_buffer;
    jpeg_decoder_handle_t decoder;
    void *panel_buffers[3];
    uint8_t panel_buffer_index;
    bool ready;
    bool preloaded;
} m5_media_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version, header_bytes, profile_count, page_count, asset_count, buttons_per_page;
    uint16_t macro_count, radial_count;
    uint32_t step_count, profiles_offset, pages_offset, assets_offset, button_macro_refs_offset;
    uint32_t macro_descriptors_offset, macro_steps_offset, blob_offset, flags;
    uint32_t radial_descriptors_offset, radial_items_offset, radial_item_count, settings;
} m5_ui_header_t;

typedef struct __attribute__((packed)) { uint16_t first_page, page_count; uint32_t reserved; } m5_ui_profile_t;
typedef struct __attribute__((packed)) { uint32_t radial_index; uint16_t asset_index; uint8_t action, fit; } m5_ui_button_t;
typedef struct __attribute__((packed)) {
    uint32_t static_offset, static_length, animation_offset, animation_length;
    uint16_t frame_count;
    uint8_t type, fps;
} m5_ui_asset_t;
typedef struct __attribute__((packed)) { uint16_t first_step, step_count; uint32_t reserved; } m5_ui_macro_t;
typedef struct __attribute__((packed)) { uint8_t kind, usage_page; uint16_t usage; uint32_t duration_ms; } m5_ui_step_t;
typedef struct __attribute__((packed)) { uint16_t first_item; uint8_t count, cover_mask; } m6_ui_radial_t;
typedef struct __attribute__((packed)) { uint16_t asset_index, macro_index; } m6_ui_radial_item_t;
typedef struct { uint32_t offset, length; } m5_icon_frame_t;
typedef struct {
    m5_icon_frame_t frames[M5_ICON_MAX_FRAMES];
    uint16_t count;
} m5_icon_index_t;

typedef struct {
    uint8_t *payload;
    size_t payload_size;
    const m5_ui_header_t *header;
    const m5_ui_profile_t *profiles;
    const m5_ui_button_t *buttons;
    const m5_ui_asset_t *assets;
    const uint16_t *button_macro_refs;
    const m5_ui_macro_t *macros;
    const m5_ui_step_t *steps;
    const m6_ui_radial_t *radials;
    const m6_ui_radial_item_t *radial_items;
    lv_image_dsc_t *images;
    m5_icon_index_t *animation_indices;
    bool ready;
} m5_ui_bundle_t;

typedef struct {
    bool active;
    uint16_t macro_index, next_step;
    int64_t due_us;
    bool tap_active;
    uint8_t tap_usage, tap_modifiers;
} m5_macro_slot_t;

typedef struct {
    lv_obj_t *image;
    uint16_t asset_index, frame;
    uint8_t frame_phase;
    lv_image_dsc_t descriptor;
} m5_visible_animation_t;

static lv_display_t *s_display;
static esp_lcd_panel_handle_t s_panel;
static lv_obj_t *s_saver_input;
static m5_media_t s_media;
static m5_ui_bundle_t s_ui_bundle;
static esp_lv_decoder_handle_t s_icon_decoder;
static uint16_t s_current_profile;
static uint16_t s_current_page;
static volatile m5_state_t s_state = M5_STATE_ACTIVE;
static volatile int64_t s_last_activity_us;
static volatile bool s_ui_ready;
static volatile bool s_wake_requested;
static volatile bool s_page_change_requested;
static volatile bool s_screensaver_requested;
static uint32_t s_screensaver_idle_seconds = M5_DEFAULT_SCREENSAVER_IDLE_SECONDS;
static uint32_t s_media_index_error;
static uint8_t s_index_buffer[M5_INDEX_BUFFER_BYTES];
static m5_macro_slot_t s_macro_slots[M5_MACRO_SLOTS];
static uint8_t s_key_refs[256], s_modifier_refs[8];
static bool s_keyboard_report_pending;
static SemaphoreHandle_t s_macro_mutex;
static TaskHandle_t s_macro_task_handle;
static m5_visible_animation_t s_visible_animations[M5_BUTTONS];
static uint8_t s_visible_animation_count;
static uint8_t s_visible_animation_cursor;
static bool s_screensaver_enabled = true;
static uint8_t s_brightness_percent = 80;
static lv_obj_t *s_radial_overlay;
static lv_obj_t *s_radial_nodes[8];
static lv_obj_t *s_radial_highlights[8];
static lv_timer_t *s_radial_prewarm_timer;
static uint16_t s_radial_prewarm_assets[M6_RADIAL_PREWARM_MAX];
static uint16_t s_radial_prewarm_count;
static uint16_t s_radial_prewarm_cursor;
static int64_t s_radial_prewarm_started_us;
static m6_radial_geometry_t s_radial_geometry;
static int8_t s_radial_selection = -1;
static uint32_t s_active_radial = UINT32_MAX;
static uint32_t s_pending_radial = UINT32_MAX;
static uint8_t s_pending_radial_button;
static m6_point_t s_pending_radial_origin;
static m6_point_t s_radial_press_point;
static bool s_radial_suppress_click;
extern const char *m3_active_bundle_path(void);

static const char *const s_symbols[M5_BUTTONS] = {
    LV_SYMBOL_PLAY, LV_SYMBOL_STOP, LV_SYMBOL_SETTINGS, LV_SYMBOL_LOOP,
    LV_SYMBOL_CHARGE, LV_SYMBOL_POWER, LV_SYMBOL_EYE_OPEN, LV_SYMBOL_HOME,
    LV_SYMBOL_SAVE, LV_SYMBOL_DOWNLOAD, LV_SYMBOL_UPLOAD, LV_SYMBOL_BELL,
    LV_SYMBOL_LEFT, LV_SYMBOL_RIGHT, LV_SYMBOL_REFRESH, LV_SYMBOL_OK,
    LV_SYMBOL_PLAY, LV_SYMBOL_STOP, LV_SYMBOL_SETTINGS, LV_SYMBOL_LOOP,
    LV_SYMBOL_CHARGE, LV_SYMBOL_POWER, LV_SYMBOL_EYE_OPEN, LV_SYMBOL_HOME,
    LV_SYMBOL_SAVE, LV_SYMBOL_DOWNLOAD, LV_SYMBOL_UPLOAD, LV_SYMBOL_BELL,
    LV_SYMBOL_LEFT, LV_SYMBOL_RIGHT, LV_SYMBOL_REFRESH, LV_SYMBOL_OK,
};

static void m5_render_active_ui(void);

static void m6_cancel_radial_prewarm(void)
{
    if (s_radial_prewarm_timer != NULL) lv_timer_delete(s_radial_prewarm_timer);
    s_radial_prewarm_timer = NULL;
    s_radial_prewarm_count = 0;
    s_radial_prewarm_cursor = 0;
    s_radial_prewarm_started_us = 0;
}

static bool m6_radial_prewarm_contains(uint16_t asset)
{
    for (uint16_t i = 0; i < s_radial_prewarm_count; ++i) {
        if (s_radial_prewarm_assets[i] == asset) return true;
    }
    return false;
}

static void m6_radial_prewarm_cb(lv_timer_t *timer)
{
    if (!s_ui_bundle.ready || s_state != M5_STATE_ACTIVE ||
        s_radial_prewarm_cursor >= s_radial_prewarm_count) {
        const uint16_t warmed = s_radial_prewarm_cursor;
        const int64_t elapsed = s_radial_prewarm_started_us == 0
            ? 0 : esp_timer_get_time() - s_radial_prewarm_started_us;
        lv_timer_delete(timer);
        s_radial_prewarm_timer = NULL;
        ESP_LOGI(TAG, "M6_PREWARM page=%u assets=%u elapsed_us=%lld",
                 s_current_page, warmed, (long long) elapsed);
        return;
    }

    if (s_radial_prewarm_cursor == 0) {
        s_radial_prewarm_started_us = esp_timer_get_time();
        lv_timer_set_period(timer, M6_RADIAL_PREWARM_PERIOD_MS);
    }
    const uint16_t asset = s_radial_prewarm_assets[s_radial_prewarm_cursor++];
    lv_image_decoder_dsc_t decoder;
    if (lv_image_decoder_open(&decoder, &s_ui_bundle.images[asset], NULL) == LV_RESULT_OK) {
        lv_image_decoder_close(&decoder);
    } else {
        ESP_LOGW(TAG, "M6_PREWARM page=%u asset=%u result=decode_failed", s_current_page, asset);
    }
}

static void m6_schedule_radial_prewarm(void)
{
    m6_cancel_radial_prewarm();
    if (!s_ui_bundle.ready) return;

    const m5_ui_button_t *page = &s_ui_bundle.buttons[(size_t) s_current_page * M5_BUTTONS];
    for (uint8_t button_index = 0; button_index < M5_BUTTONS; ++button_index) {
        const uint32_t radial_index = page[button_index].radial_index;
        if (radial_index == UINT32_MAX) continue;
        const m6_ui_radial_t *radial = &s_ui_bundle.radials[radial_index];
        for (uint8_t item_index = 0; item_index < radial->count; ++item_index) {
            const uint16_t asset = s_ui_bundle.radial_items[radial->first_item + item_index].asset_index;
            if (asset == UINT16_MAX || m6_radial_prewarm_contains(asset)) continue;
            if (s_radial_prewarm_count < M6_RADIAL_PREWARM_MAX) {
                s_radial_prewarm_assets[s_radial_prewarm_count++] = asset;
            }
        }
    }
    if (s_radial_prewarm_count != 0) {
        s_radial_prewarm_timer = lv_timer_create(m6_radial_prewarm_cb,
                                                  M6_RADIAL_PREWARM_INITIAL_MS, NULL);
    }
}

static void m5_touch_activity(void)
{
    s_last_activity_us = esp_timer_get_time();
}

static bool m5_range_valid(size_t offset, size_t count, size_t item_size, size_t total)
{
    return offset <= total && item_size != 0 && count <= (total - offset) / item_size;
}

static bool m5_index_icon(const uint8_t *payload, const m5_ui_asset_t *asset, m5_icon_index_t *index)
{
    bool in_frame = false;
    uint8_t previous = 0;
    uint32_t start = 0;
    for (uint32_t position = 0; position < asset->animation_length; ++position) {
        const uint8_t current = payload[asset->animation_offset + position];
        if (!in_frame && previous == 0xff && current == 0xd8) {
            start = position - 1;
            in_frame = true;
        } else if (in_frame && previous == 0xff && current == 0xd9) {
            if (index->count >= M5_ICON_MAX_FRAMES) return false;
            index->frames[index->count++] = (m5_icon_frame_t) {
                .offset = asset->animation_offset + start,
                .length = position + 1 - start,
            };
            in_frame = false;
        }
        previous = current;
    }
    return !in_frame && index->count == asset->frame_count && index->count > 1;
}

static bool m5_load_ui_bundle(void)
{
    const char *path = m3_active_bundle_path();
    if (path == NULL) return false;
    FILE *file = fopen(path, "rb");
    uint8_t sdb_header[16];
    if (file == NULL || fread(sdb_header, 1, sizeof(sdb_header), file) != sizeof(sdb_header)) {
        if (file) fclose(file);
        return false;
    }
    uint32_t magic, total_bytes;
    memcpy(&magic, sdb_header, 4); memcpy(&total_bytes, sdb_header + 8, 4);
    if (magic != M5_SDB3_MAGIC || total_bytes < 16 + sizeof(m5_ui_header_t) || total_bytes > 16U * 1024U * 1024U) {
        fclose(file); return false;
    }
    const size_t payload_size = total_bytes - 16U;
    uint8_t *payload = heap_caps_malloc(payload_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (payload == NULL || fread(payload, 1, payload_size, file) != payload_size) {
        heap_caps_free(payload); fclose(file); return false;
    }
    fclose(file);
    const m5_ui_header_t *header = (const m5_ui_header_t *) payload;
    if (header->magic != M5_UI_MAGIC || header->version != 3 || header->header_bytes != sizeof(*header) ||
        header->profile_count == 0 || header->page_count == 0 || header->buttons_per_page != M5_BUTTONS ||
        !m5_range_valid(header->profiles_offset, header->profile_count, sizeof(m5_ui_profile_t), payload_size) ||
        !m5_range_valid(header->pages_offset, (size_t) header->page_count * M5_BUTTONS, sizeof(m5_ui_button_t), payload_size) ||
        !m5_range_valid(header->assets_offset, header->asset_count, sizeof(m5_ui_asset_t), payload_size) ||
        !m5_range_valid(header->button_macro_refs_offset, (size_t) header->page_count * M5_BUTTONS, sizeof(uint16_t), payload_size) ||
        !m5_range_valid(header->macro_descriptors_offset, header->macro_count, sizeof(m5_ui_macro_t), payload_size) ||
        !m5_range_valid(header->macro_steps_offset, header->step_count, sizeof(m5_ui_step_t), payload_size) ||
        !m5_range_valid(header->radial_descriptors_offset, header->radial_count, sizeof(m6_ui_radial_t), payload_size) ||
        !m5_range_valid(header->radial_items_offset, header->radial_item_count, sizeof(m6_ui_radial_item_t), payload_size) ||
        header->blob_offset > payload_size) {
        heap_caps_free(payload); return false;
    }
    const m5_ui_profile_t *profiles = (const m5_ui_profile_t *) (payload + header->profiles_offset);
    const m5_ui_button_t *buttons = (const m5_ui_button_t *) (payload + header->pages_offset);
    const m5_ui_asset_t *assets = (const m5_ui_asset_t *) (payload + header->assets_offset);
    const uint16_t *button_macro_refs = (const uint16_t *) (payload + header->button_macro_refs_offset);
    const m5_ui_macro_t *macros = (const m5_ui_macro_t *) (payload + header->macro_descriptors_offset);
    const m5_ui_step_t *steps = (const m5_ui_step_t *) (payload + header->macro_steps_offset);
    const m6_ui_radial_t *radials = (const m6_ui_radial_t *) (payload + header->radial_descriptors_offset);
    const m6_ui_radial_item_t *radial_items = (const m6_ui_radial_item_t *) (payload + header->radial_items_offset);
    for (uint16_t i = 0; i < header->profile_count; ++i) {
        if (profiles[i].page_count == 0 || profiles[i].first_page + profiles[i].page_count > header->page_count) {
            heap_caps_free(payload); return false;
        }
    }
    lv_image_dsc_t *images = calloc(header->asset_count, sizeof(lv_image_dsc_t));
    m5_icon_index_t *indices = calloc(header->asset_count, sizeof(m5_icon_index_t));
    if (header->asset_count != 0 && (images == NULL || indices == NULL)) { free(images); free(indices); heap_caps_free(payload); return false; }
    for (uint16_t i = 0; i < header->asset_count; ++i) {
        if (!m5_range_valid(assets[i].static_offset, assets[i].static_length, 1, payload_size) || assets[i].static_offset < header->blob_offset ||
            (assets[i].type == 2 && (assets[i].fps != M5_ICON_FPS ||
             !m5_range_valid(assets[i].animation_offset, assets[i].animation_length, 1, payload_size) ||
             assets[i].animation_offset < header->blob_offset || !m5_index_icon(payload, &assets[i], &indices[i])))) {
            free(images); free(indices); heap_caps_free(payload); return false;
        }
        images[i].data = payload + assets[i].static_offset;
        images[i].data_size = assets[i].static_length;
    }
    for (uint16_t i = 0; i < header->macro_count; ++i) {
        if ((uint32_t) macros[i].first_step + macros[i].step_count > header->step_count) {
            free(images); free(indices); heap_caps_free(payload); return false;
        }
    }
    for (size_t i = 0; i < (size_t) header->page_count * M5_BUTTONS; ++i) {
        if (buttons[i].asset_index != UINT16_MAX && buttons[i].asset_index >= header->asset_count) {
            free(images); free(indices); heap_caps_free(payload); return false;
        }
        if (button_macro_refs[i] != UINT16_MAX && button_macro_refs[i] >= header->macro_count) {
            free(images); free(indices); heap_caps_free(payload); return false;
        }
        if (buttons[i].radial_index != UINT32_MAX && buttons[i].radial_index >= header->radial_count) {
            free(images); free(indices); heap_caps_free(payload); return false;
        }
    }
    for (uint16_t i = 0; i < header->radial_count; ++i) {
        if ((radials[i].count != 4 && radials[i].count != 6 && radials[i].count != 8) ||
            (uint32_t) radials[i].first_item + radials[i].count > header->radial_item_count) {
            free(images); free(indices); heap_caps_free(payload); return false;
        }
    }
    for (uint32_t i = 0; i < header->radial_item_count; ++i) {
        const uint16_t action_ref = radial_items[i].macro_index;
        if ((action_ref >= header->macro_count && action_ref < M6_RADIAL_ACTION_PROFILE_NEXT) ||
            (radial_items[i].asset_index != UINT16_MAX && radial_items[i].asset_index >= header->asset_count)) {
            free(images); free(indices); heap_caps_free(payload); return false;
        }
    }
    s_ui_bundle = (m5_ui_bundle_t) {
        .payload = payload, .payload_size = payload_size, .header = header,
        .profiles = profiles, .buttons = buttons, .assets = assets,
        .button_macro_refs = button_macro_refs, .macros = macros, .steps = steps,
        .radials = radials, .radial_items = radial_items,
        .images = images, .animation_indices = indices, .ready = true,
    };
    s_current_profile = 0;
    s_current_page = profiles[0].first_page;
    s_screensaver_idle_seconds = (header->flags >= 5 && header->flags <= 3600)
        ? header->flags : M5_DEFAULT_SCREENSAVER_IDLE_SECONDS;
    s_brightness_percent = (header->settings & 0xff) <= 100 ? header->settings & 0xff : 80;
    s_screensaver_enabled = (header->settings & (1U << 9)) != 0;
    ESP_ERROR_CHECK(bsp_display_brightness_set(s_brightness_percent));
    lv_display_set_rotation(s_display, (header->settings & (1U << 8)) ? LV_DISPLAY_ROTATION_180 : LV_DISPLAY_ROTATION_0);
    ESP_LOGI(TAG, "M6_UI bundle=loaded schema=3 profiles=%u pages=%u assets=%u macros=%u radials=%u bytes=%u",
             header->profile_count, header->page_count, header->asset_count, header->macro_count, header->radial_count, (unsigned) payload_size);
    return true;
}

static void m5_input_event_cb(lv_event_t *event)
{
    const lv_event_code_t code = lv_event_get_code(event);
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_CLICKED) {
        return;
    }
    m5_touch_activity();
    if (s_state == M5_STATE_PLAYING && code == LV_EVENT_PRESSED) {
        /* Never clean the screen from the callback of the object being
         * deleted. The media task performs the transition after LVGL has
         * returned from input dispatch; this also consumes the wake press. */
        s_state = M5_STATE_WAKING;
        s_wake_requested = true;
    }
}

static bool m5_flush_keyboard_locked(void)
{
    if (!s_keyboard_report_pending) return true;
    uint8_t report[6] = {0};
    uint8_t modifiers = 0, count = 0;
    for (uint8_t i = 0; i < 8; ++i) if (s_modifier_refs[i]) modifiers |= 1U << i;
    for (uint16_t usage = 1; usage < 256 && count < 6; ++usage) {
        if (s_key_refs[usage]) report[count++] = (uint8_t) usage;
    }
    if (!tud_hid_ready() ||
        !tud_hid_keyboard_report(M5_HID_KEYBOARD_REPORT_ID, modifiers, report)) {
        return false;
    }
    s_keyboard_report_pending = false;
    return true;
}

static void m5_emit_keyboard_locked(void)
{
    /* A release report is safety-critical. If the interrupt endpoint is busy,
     * retain the newest complete keyboard state and let the macro task retry it
     * instead of silently leaving a key or modifier held on the host. */
    s_keyboard_report_pending = true;
    (void) m5_flush_keyboard_locked();
}

void m5_hid_release_all(const char *reason)
{
    if (s_macro_mutex == NULL || xSemaphoreTake(s_macro_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
    memset(s_key_refs, 0, sizeof(s_key_refs));
    memset(s_modifier_refs, 0, sizeof(s_modifier_refs));
    memset(s_macro_slots, 0, sizeof(s_macro_slots));
    m5_emit_keyboard_locked();
    if (tud_hid_ready()) {
        const uint16_t released = 0;
        tud_hid_report(M5_HID_CONSUMER_REPORT_ID, &released, sizeof(released));
    }
    xSemaphoreGive(s_macro_mutex);
    if (s_macro_task_handle != NULL) xTaskNotifyGive(s_macro_task_handle);
    ESP_LOGI(TAG, "M5_HID release_all reason=%s", reason ? reason : "unspecified");
}

static void m5_start_macro(uint16_t macro_index)
{
    if (macro_index >= s_ui_bundle.header->macro_count || xSemaphoreTake(s_macro_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return;
    m5_macro_slot_t *slot = NULL;
    for (size_t i = 0; i < M5_MACRO_SLOTS; ++i) {
        if (s_macro_slots[i].active && s_macro_slots[i].macro_index == macro_index) {
            memset(s_key_refs, 0, sizeof(s_key_refs));
            memset(s_modifier_refs, 0, sizeof(s_modifier_refs));
            m5_emit_keyboard_locked();
            slot = &s_macro_slots[i];
            break;
        }
        if (!slot && !s_macro_slots[i].active) slot = &s_macro_slots[i];
    }
    if (slot) *slot = (m5_macro_slot_t) {.active = true, .macro_index = macro_index, .due_us = esp_timer_get_time()};
    xSemaphoreGive(s_macro_mutex);
    if (slot && s_macro_task_handle != NULL) xTaskNotifyGive(s_macro_task_handle);
    ESP_LOGI(TAG, "M5_MACRO start=%u result=%s", macro_index, slot ? "queued" : "slots_full");
}

static void m5_macro_task(void *argument)
{
    (void) argument;
    for (;;) {
        TickType_t wait_ticks = portMAX_DELAY;
        if (xSemaphoreTake(s_macro_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            const int64_t now = esp_timer_get_time();
            (void) m5_flush_keyboard_locked();
            for (size_t i = 0; s_ui_bundle.ready && i < M5_MACRO_SLOTS; ++i) {
                m5_macro_slot_t *slot = &s_macro_slots[i];
                if (!slot->active || now < slot->due_us) continue;
                if (slot->tap_active) {
                    if (s_key_refs[slot->tap_usage]) --s_key_refs[slot->tap_usage];
                    for (uint8_t bit = 0; bit < 4; ++bit) if ((slot->tap_modifiers & (1U << bit)) && s_modifier_refs[bit]) --s_modifier_refs[bit];
                    slot->tap_active = false;
                    m5_emit_keyboard_locked();
                    continue;
                }
                const m5_ui_macro_t *macro = &s_ui_bundle.macros[slot->macro_index];
                if (slot->next_step >= macro->step_count) { slot->active = false; continue; }
                const m5_ui_step_t *step = &s_ui_bundle.steps[macro->first_step + slot->next_step++];
                if (step->kind == 3) {
                    slot->due_us = now + (int64_t) step->duration_ms * 1000;
                } else if (step->kind == 4 && step->usage_page == 0x0c) {
                    if (tud_hid_ready()) {
                        tud_hid_report(M5_HID_CONSUMER_REPORT_ID, &step->usage, sizeof(step->usage));
                        const uint16_t released = 0;
                        tud_hid_report(M5_HID_CONSUMER_REPORT_ID, &released, sizeof(released));
                    }
                } else if (step->kind == 5 && step->usage < 0xe0 && step->usage < 256) {
                    if (s_key_refs[step->usage] != UINT8_MAX) ++s_key_refs[step->usage];
                    for (uint8_t bit = 0; bit < 4; ++bit) if ((step->usage_page & (1U << bit)) && s_modifier_refs[bit] != UINT8_MAX) ++s_modifier_refs[bit];
                    slot->tap_active = true;
                    slot->tap_usage = (uint8_t) step->usage;
                    slot->tap_modifiers = step->usage_page & 0x0f;
                    slot->due_us = now + (int64_t) (step->duration_ms ? step->duration_ms : 25) * 1000;
                    m5_emit_keyboard_locked();
                } else if ((step->kind == 1 || step->kind == 2) && step->usage_page == 0x07 && step->usage < 256) {
                    uint8_t *ref = step->usage >= 0xe0 && step->usage <= 0xe7
                        ? &s_modifier_refs[step->usage - 0xe0] : &s_key_refs[step->usage];
                    if (step->kind == 1 && *ref != UINT8_MAX) ++*ref;
                    if (step->kind == 2 && *ref) --*ref;
                    m5_emit_keyboard_locked();
                }
            }
            const int64_t after = esp_timer_get_time();
            for (size_t i = 0; i < M5_MACRO_SLOTS; ++i) {
                const m5_macro_slot_t *slot = &s_macro_slots[i];
                if (!slot->active) continue;
                const int64_t remaining_us = slot->due_us - after;
                const TickType_t candidate = remaining_us <= 0 ? 1 : pdMS_TO_TICKS((remaining_us + 999) / 1000);
                if (wait_ticks == portMAX_DELAY || candidate < wait_ticks) wait_ticks = candidate;
            }
            if (s_keyboard_report_pending && (wait_ticks == portMAX_DELAY || wait_ticks > 1)) {
                wait_ticks = 1;
            }
            xSemaphoreGive(s_macro_mutex);
        }
        ulTaskNotifyTake(pdTRUE, wait_ticks);
    }
}

static m6_point_t m6_touch_point(void)
{
    lv_point_t point = {0};
    lv_indev_t *indev = lv_indev_active();
    if (indev != NULL) lv_indev_get_point(indev, &point);
    return (m6_point_t) {(int16_t) point.x, (int16_t) point.y};
}

static bool m6_radial_drag_exceeded(m6_point_t point)
{
    const int16_t dx = abs(point.x - s_radial_press_point.x);
    const int16_t dy = abs(point.y - s_radial_press_point.y);
    return (dx > dy ? dx : dy) >= M6_RADIAL_OPEN_DRAG_PX;
}

static m6_point_t m6_radial_grid_offset(uint8_t count, uint8_t index, int16_t step)
{
    static const int8_t offsets4[4][2] = {{0,-2},{2,0},{0,2},{-2,0}};
    static const int8_t offsets6[6][2] = {{0,-2},{2,-1},{2,1},{0,2},{-2,1},{-2,-1}};
    static const int8_t offsets8[8][2] = {{0,-2},{2,-2},{2,0},{2,2},{0,2},{-2,2},{-2,0},{-2,-2}};
    const int8_t (*offsets)[2] = count == 4 ? offsets4 : count == 6 ? offsets6 : offsets8;
    return (m6_point_t) {(int16_t) (offsets[index][0] * step / 2),
                         (int16_t) (offsets[index][1] * step / 2)};
}

static const char *m6_radial_direction_label(uint8_t count, uint8_t index)
{
    static const char *const labels4[4] = {"N", "E", "S", "W"};
    static const char *const labels6[6] = {"N", "NE", "SE", "S", "SW", "NW"};
    static const char *const labels8[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    return (count == 4 ? labels4 : count == 6 ? labels6 : labels8)[index];
}

static void m5_run_ui_action(uint8_t action, uint16_t macro_index)
{
    const m5_ui_profile_t *profile = &s_ui_bundle.profiles[s_current_profile];
    if (action == 1) {
        if (macro_index != UINT16_MAX) m5_start_macro(macro_index);
    } else if (action == 2) {
        const uint16_t last = profile->first_page + profile->page_count - 1;
        s_current_page = s_current_page >= last ? profile->first_page : s_current_page + 1;
        s_page_change_requested = true;
    } else if (action == 3) {
        s_current_page = s_current_page <= profile->first_page
            ? profile->first_page + profile->page_count - 1 : s_current_page - 1;
        s_page_change_requested = true;
    } else if (action == 4) {
        s_current_profile = (s_current_profile + 1) % s_ui_bundle.header->profile_count;
        s_current_page = s_ui_bundle.profiles[s_current_profile].first_page;
        s_page_change_requested = true;
    }
}

static void m6_run_radial_action(const m6_ui_radial_item_t *item)
{
    const uint16_t action_ref = item->macro_index;
    if (action_ref < s_ui_bundle.header->macro_count) m5_run_ui_action(1, action_ref);
    else if (action_ref == M6_RADIAL_ACTION_PAGE_NEXT) m5_run_ui_action(2, UINT16_MAX);
    else if (action_ref == M6_RADIAL_ACTION_PAGE_PREVIOUS) m5_run_ui_action(3, UINT16_MAX);
    else if (action_ref == M6_RADIAL_ACTION_PROFILE_NEXT) m5_run_ui_action(4, UINT16_MAX);
}

static void m6_close_radial(bool run_selection)
{
    if (s_active_radial != UINT32_MAX && run_selection && s_radial_selection >= 0) {
        const m6_ui_radial_t *radial = &s_ui_bundle.radials[s_active_radial];
        const m6_ui_radial_item_t *item = &s_ui_bundle.radial_items[radial->first_item + s_radial_selection];
        m6_run_radial_action(item);
        ESP_LOGI(TAG, "M6_RADIAL selection=%d action_ref=%u committed=1", s_radial_selection, item->macro_index);
    }
    if (s_radial_overlay != NULL) lv_obj_delete(s_radial_overlay);
    s_radial_overlay = NULL;
    memset(s_radial_nodes, 0, sizeof(s_radial_nodes));
    memset(s_radial_highlights, 0, sizeof(s_radial_highlights));
    s_active_radial = UINT32_MAX;
    s_pending_radial = UINT32_MAX;
    /* Do not clear s_radial_suppress_click here. LVGL emits CLICKED after the
     * RELEASED event that closes this overlay, so the source tile must retain
     * the flag long enough to consume that follow-up click. PRESS_LOST clears
     * it explicitly because no center click follows a cancelled gesture. */
    s_radial_selection = -1;
    if (s_radial_prewarm_timer != NULL) lv_timer_resume(s_radial_prewarm_timer);
}

static void m6_open_radial(uint32_t radial_index, uint8_t button_index, m6_point_t origin)
{
    const m6_ui_radial_t *radial = &s_ui_bundle.radials[radial_index];
    const uint16_t width = lv_display_get_horizontal_resolution(s_display);
    const uint16_t height = lv_display_get_vertical_resolution(s_display);
    const int16_t tile_size = (width - (2 * M5_MARGIN_X_PX) - ((M5_COLUMNS - 1) * M5_GAP_PX)) / M5_COLUMNS;
    const int16_t step = tile_size + M5_GAP_PX;
    s_radial_geometry = m6_radial_place(origin, width, height, radial->count);
    /* Keep both gesture selection and the visual menu anchored to the key
     * centre. Edge items may be clipped by the display. */
    s_radial_geometry.center = origin;
    s_active_radial = radial_index;
    s_radial_selection = -1;
    if (s_radial_prewarm_timer != NULL) lv_timer_pause(s_radial_prewarm_timer);
    s_radial_overlay = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_radial_overlay);
    lv_obj_set_size(s_radial_overlay, width, height);
    lv_obj_set_pos(s_radial_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_radial_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_radial_overlay, LV_OPA_50, 0);
    lv_obj_remove_flag(s_radial_overlay, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(s_radial_overlay);
    for (uint8_t i = 0; i < radial->count; ++i) {
        const m6_point_t offset = m6_radial_grid_offset(radial->count, i, step);
        lv_obj_t *node = lv_obj_create(s_radial_overlay);
        s_radial_nodes[i] = node;
        lv_obj_remove_style_all(node);
        lv_obj_set_size(node, tile_size, tile_size);
        lv_obj_set_pos(node, origin.x + offset.x - tile_size / 2,
                       origin.y + offset.y - tile_size / 2);
        lv_obj_set_style_radius(node, 0, 0);
        lv_obj_set_style_bg_color(node, lv_color_hex(0x101010), 0);
        lv_obj_set_style_bg_opa(node, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(node, 0, 0);
        lv_obj_remove_flag(node, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *label = lv_label_create(node);
        lv_label_set_text(label, m6_radial_direction_label(radial->count, i));
        lv_obj_set_style_text_color(label, lv_color_hex(0xD0D0D0), 0);
        lv_obj_center(label);
        const uint16_t asset = s_ui_bundle.radial_items[radial->first_item + i].asset_index;
        if (asset != UINT16_MAX) {
            lv_obj_t *image = lv_image_create(node);
            lv_image_set_src(image, &s_ui_bundle.images[asset]);
            lv_obj_set_size(image, tile_size, tile_size);
            lv_image_set_inner_align(image, (radial->cover_mask & (1U << i))
                ? LV_IMAGE_ALIGN_COVER : LV_IMAGE_ALIGN_CONTAIN);
            lv_obj_center(image);
        }
        lv_obj_t *highlight = lv_obj_create(node);
        s_radial_highlights[i] = highlight;
        lv_obj_remove_style_all(highlight);
        lv_obj_set_size(highlight, tile_size, tile_size);
        lv_obj_center(highlight);
        lv_obj_set_style_bg_color(highlight, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_bg_opa(highlight, LV_OPA_20, 0);
        lv_obj_remove_flag(highlight, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(highlight, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_t *center = lv_obj_create(s_radial_overlay);
    lv_obj_remove_style_all(center);
    lv_obj_set_size(center, tile_size, tile_size);
    lv_obj_set_pos(center, origin.x - tile_size / 2,
                   origin.y - tile_size / 2);
    lv_obj_set_style_radius(center, 0, 0);
    lv_obj_set_style_bg_color(center, lv_color_hex(0x101010), 0);
    lv_obj_set_style_bg_opa(center, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(center, 0, 0);
    lv_obj_remove_flag(center, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    const m5_ui_button_t *source = &s_ui_bundle.buttons[(size_t) s_current_page * M5_BUTTONS + button_index];
    if (source->asset_index != UINT16_MAX) {
        lv_obj_t *image = lv_image_create(center);
        lv_image_set_src(image, &s_ui_bundle.images[source->asset_index]);
        lv_obj_set_size(image, tile_size, tile_size);
        lv_image_set_inner_align(image, source->fit ? LV_IMAGE_ALIGN_CONTAIN : LV_IMAGE_ALIGN_COVER);
        lv_obj_center(image);
    }
}

static void m6_update_radial(m6_point_t point)
{
    if (s_active_radial == UINT32_MAX) return;
    const m6_radial_selection_t next = m6_radial_select(&s_radial_geometry, point, s_radial_selection);
    if (next.index == s_radial_selection) return;
    if (s_radial_selection >= 0 && s_radial_highlights[s_radial_selection] != NULL)
        lv_obj_add_flag(s_radial_highlights[s_radial_selection], LV_OBJ_FLAG_HIDDEN);
    s_radial_selection = next.index;
    if (s_radial_selection >= 0 && s_radial_highlights[s_radial_selection] != NULL)
        lv_obj_remove_flag(s_radial_highlights[s_radial_selection], LV_OBJ_FLAG_HIDDEN);
}

static void m5_tile_event_cb(lv_event_t *event)
{
    m5_input_event_cb(event);
    if (s_state != M5_STATE_ACTIVE || !s_ui_bundle.ready) return;
    const lv_event_code_t code = lv_event_get_code(event);
    const uint32_t button_index = (uint32_t) (uintptr_t) lv_event_get_user_data(event);
    if (button_index >= M5_BUTTONS) return;
    const m5_ui_button_t *button = &s_ui_bundle.buttons[(size_t) s_current_page * M5_BUTTONS + button_index];
    lv_obj_t *tile = lv_event_get_target_obj(event);
    if ((code == LV_EVENT_PRESSED || code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) &&
        lv_obj_get_child_count(tile) != 0) {
        /* The final child is a paint-only press overlay. Unlike a parent
         * outline, it is drawn after a full-bleed icon and stays visible. */
        lv_obj_t *overlay = lv_obj_get_child(tile, -1);
        if (code == LV_EVENT_PRESSED) {
            lv_obj_remove_flag(overlay, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (button->radial_index != UINT32_MAX) {
        if (code == LV_EVENT_PRESSED) {
            lv_area_t area;
            lv_obj_get_coords(tile, &area);
            s_pending_radial_origin = (m6_point_t) {
                (int16_t) ((area.x1 + area.x2) / 2),
                (int16_t) ((area.y1 + area.y2) / 2),
            };
            s_radial_press_point = m6_touch_point();
            s_pending_radial = button->radial_index;
            s_pending_radial_button = button_index;
            s_radial_suppress_click = false;
        } else if (code == LV_EVENT_PRESSING) {
            const m6_point_t point = m6_touch_point();
            if (s_active_radial != UINT32_MAX) {
                m6_update_radial(point);
            } else if (s_pending_radial == button->radial_index && m6_radial_drag_exceeded(point)) {
                s_radial_suppress_click = true;
                m6_open_radial(s_pending_radial, s_pending_radial_button, s_pending_radial_origin);
                m6_update_radial(point);
            }
        } else if (code == LV_EVENT_RELEASED) {
            if (s_active_radial != UINT32_MAX) {
                const m6_radial_selection_t final = m6_radial_select(&s_radial_geometry, m6_touch_point(), s_radial_selection);
                s_radial_selection = final.index;
                m6_close_radial(final.committed);
            }
            s_pending_radial = UINT32_MAX;
        } else if (code == LV_EVENT_PRESS_LOST) {
            if (s_active_radial != UINT32_MAX) m6_close_radial(false);
            s_pending_radial = UINT32_MAX;
            s_radial_suppress_click = false;
        }
    }
    if (code != LV_EVENT_CLICKED) return;
    if (s_radial_suppress_click) {
        s_radial_suppress_click = false;
        return;
    }
    const uint16_t macro = s_ui_bundle.button_macro_refs[(size_t) s_current_page * M5_BUTTONS + button_index];
    m5_run_ui_action(button->action, macro);
}

static bool m5_enter_saver_ui(void)
{
    lv_obj_t *screen = lv_screen_active();
    m6_cancel_radial_prewarm();
    lv_obj_clean(screen);
    lv_image_cache_drop(NULL);
    if (s_icon_decoder != NULL) {
        if (esp_lv_decoder_deinit(s_icon_decoder) != ESP_OK) return false;
        s_icon_decoder = NULL;
    }
    if (s_media.decoder == NULL && jpeg_new_decoder_engine(&(jpeg_decode_engine_cfg_t) {
            .intr_priority = 1, .timeout_ms = 100,
        }, &s_media.decoder) != ESP_OK) {
        ESP_LOGE(TAG, "M5_MEDIA result=decoder_handoff_failed owner=screensaver");
        ESP_ERROR_CHECK(esp_lv_decoder_init(&s_icon_decoder));
        m5_render_active_ui();
        return false;
    }
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    s_visible_animation_count = 0;
    s_saver_input = lv_button_create(screen);
    lv_obj_set_size(s_saver_input, lv_display_get_horizontal_resolution(s_display),
                    lv_display_get_vertical_resolution(s_display));
    lv_obj_set_pos(s_saver_input, 0, 0);
    lv_obj_set_style_bg_opa(s_saver_input, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_saver_input, 0, 0);
    lv_obj_add_event_cb(s_saver_input, m5_input_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_flag(s_saver_input, LV_OBJ_FLAG_CLICKABLE);
    s_state = M5_STATE_PLAYING;
    ESP_LOGI(TAG, "M5_STATE from=active to=playing frames=%u source=%s",
             s_media.frame_count, s_media.preloaded ? "psram" : "sd");
    return true;
}

static void m5_render_active_ui(void)
{
    if (s_display == NULL) return;
    m6_cancel_radial_prewarm();
    s_radial_overlay = NULL;
    memset(s_radial_nodes, 0, sizeof(s_radial_nodes));
    memset(s_radial_highlights, 0, sizeof(s_radial_highlights));
    s_active_radial = UINT32_MAX;
    s_radial_selection = -1;
    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
    s_visible_animation_count = 0;
    s_visible_animation_cursor = 0;
    s_saver_input = NULL;
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    const int width = lv_display_get_horizontal_resolution(s_display);
    const int height = lv_display_get_vertical_resolution(s_display);
    const int tile_size = (width - (2 * M5_MARGIN_X_PX) - ((M5_COLUMNS - 1) * M5_GAP_PX)) / M5_COLUMNS;
    const int grid_height = (M5_ROWS * tile_size) + ((M5_ROWS - 1) * M5_GAP_PX);
    const int top = (height - grid_height) / 2;

    for (uint8_t row = 0; row < M5_ROWS; ++row) {
        for (uint8_t column = 0; column < M5_COLUMNS; ++column) {
            const uint8_t index = row * M5_COLUMNS + column;
            lv_obj_t *tile = lv_button_create(screen);
            lv_obj_set_size(tile, tile_size, tile_size);
            lv_obj_set_pos(tile, M5_MARGIN_X_PX + column * (tile_size + M5_GAP_PX),
                           top + row * (tile_size + M5_GAP_PX));
            lv_obj_set_style_radius(tile, 12, 0);
            lv_obj_set_style_border_width(tile, 0, 0);
            lv_obj_set_style_shadow_width(tile, 0, 0);
            lv_obj_set_style_pad_all(tile, 0, 0);
            /* The default LVGL button theme applies a black recolor to the
             * entire button subtree while pressed. Our feedback is the icon
             * scale change instead, so explicitly override that theme style. */
            lv_obj_set_style_recolor_opa(tile, LV_OPA_TRANSP, LV_STATE_PRESSED);
            lv_obj_set_style_transform_width(tile, 0, LV_STATE_PRESSED);
            lv_obj_set_style_transform_height(tile, 0, LV_STATE_PRESSED);
            const m5_ui_button_t *definition = s_ui_bundle.ready
                ? &s_ui_bundle.buttons[(size_t) s_current_page * M5_BUTTONS + index] : NULL;
            lv_obj_set_style_bg_color(tile, lv_color_hex(0x202126), 0);
            lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
            lv_obj_add_event_cb(tile, m5_tile_event_cb, LV_EVENT_ALL, (void *) (uintptr_t) index);
            lv_obj_t *icon;
            if (definition && definition->asset_index != UINT16_MAX) {
                const uint16_t asset_index = definition->asset_index;
                bool animation_has_baked_corners = false;
                if (s_ui_bundle.assets[asset_index].type == 2) {
                    const m5_icon_frame_t *first = &s_ui_bundle.animation_indices[asset_index].frames[0];
                    jpeg_decode_picture_info_t info;
                    animation_has_baked_corners =
                        jpeg_decoder_get_info(s_ui_bundle.payload + first->offset, first->length, &info) == ESP_OK &&
                        info.width == tile_size && info.height == tile_size;
                }
                icon = lv_image_create(tile);
                lv_obj_set_size(icon, tile_size, tile_size);
                /* Animated frames have their rounded #202126 corners baked
                 * by the editor. Keep clip_radius at zero for those frames so
                 * the ESP32-P4 image accelerator remains eligible. Static
                 * artwork is masked here because it redraws infrequently. */
                if (!animation_has_baked_corners) {
                    lv_obj_set_style_radius(icon, 12, 0);
                }
                lv_obj_remove_flag(icon, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_set_scrollbar_mode(icon, LV_SCROLLBAR_MODE_OFF);
                lv_image_set_src(icon, &s_ui_bundle.images[definition->asset_index]);
                lv_image_set_inner_align(icon, definition->fit ? LV_IMAGE_ALIGN_CONTAIN : LV_IMAGE_ALIGN_COVER);
                lv_obj_center(icon);
                if (s_ui_bundle.assets[asset_index].type == 2 && s_visible_animation_count < M5_BUTTONS) {
                    m5_visible_animation_t *visible = &s_visible_animations[s_visible_animation_count++];
                    *visible = (m5_visible_animation_t) {.image = icon, .asset_index = asset_index};
                    visible->descriptor.data = s_ui_bundle.payload + s_ui_bundle.animation_indices[asset_index].frames[0].offset;
                    visible->descriptor.data_size = s_ui_bundle.animation_indices[asset_index].frames[0].length;
                    lv_image_set_src(icon, &visible->descriptor);
                }
            } else if (!s_ui_bundle.ready) {
                icon = lv_label_create(tile);
                lv_label_set_text(icon, s_symbols[index]);
                lv_obj_set_style_text_color(icon, lv_color_hex(0xFFFFFF), 0);
                lv_obj_set_style_text_font(icon, &lv_font_montserrat_14, 0);
                lv_obj_center(icon);
            }
            /* Create this last so the border is composited above any icon.
             * It never participates in input or layout and only invalidates
             * its own thin border when press state changes. */
            lv_obj_t *press_overlay = lv_obj_create(tile);
            lv_obj_set_size(press_overlay, tile_size, tile_size);
            lv_obj_center(press_overlay);
            lv_obj_set_style_radius(press_overlay, 12, 0);
            lv_obj_set_style_bg_opa(press_overlay, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(press_overlay, 3, 0);
            lv_obj_set_style_border_color(press_overlay, lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_border_opa(press_overlay, LV_OPA_70, 0);
            lv_obj_set_style_pad_all(press_overlay, 0, 0);
            lv_obj_remove_flag(press_overlay, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(press_overlay, LV_OBJ_FLAG_HIDDEN);
        }
    }
    ESP_LOGI(TAG, "M5_UI grid=8x4 square_px=%d black_bars_px=%d,%d", tile_size, top,
             height - grid_height - top);
    m6_schedule_radial_prewarm();
}

static uint8_t m5_animation_target_fps(void)
{
    if (s_visible_animation_count <= 8) return M5_ICON_FPS;
    if (s_visible_animation_count <= 16) return M5_ICON_MEDIUM_LOAD_FPS;
    return M5_ICON_HEAVY_LOAD_FPS;
}

static uint8_t m5_advance_animation_batch(uint8_t target_fps)
{
    const uint8_t count = s_visible_animation_count < M5_ICON_UPDATE_BATCH
        ? s_visible_animation_count : M5_ICON_UPDATE_BATCH;
    for (uint8_t i = 0; i < count; ++i) {
        m5_visible_animation_t *visible = &s_visible_animations[s_visible_animation_cursor];
        const m5_icon_index_t *index = &s_ui_bundle.animation_indices[visible->asset_index];
        /* The source stream remains 15 FPS. At reduced display rates, skip
         * source frames with a phase accumulator so motion keeps its original
         * speed instead of playing in slow motion. */
        visible->frame_phase += M5_ICON_FPS;
        const uint8_t source_frames = visible->frame_phase / target_fps;
        visible->frame_phase %= target_fps;
        visible->frame = (visible->frame + source_frames) % index->count;
        lv_image_cache_drop(&visible->descriptor);
        visible->descriptor.data = s_ui_bundle.payload + index->frames[visible->frame].offset;
        visible->descriptor.data_size = index->frames[visible->frame].length;
        lv_image_set_src(visible->image, &visible->descriptor);
        lv_obj_invalidate(visible->image);
        s_visible_animation_cursor = (s_visible_animation_cursor + 1) % s_visible_animation_count;
    }
    return count;
}

static bool m5_index_mjpeg(void)
{
    s_media_index_error = 0;
    FILE *file = fopen(M5_JPEG_PATH, "rb");
    if (file == NULL) { s_media_index_error = 1; return false; }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        s_media_index_error = 2; return false;
    }
    const long size = ftell(file);
    if (size <= 0 || size > UINT32_MAX || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        s_media_index_error = 2; return false;
    }

    uint32_t position = 0;
    uint32_t start = 0;
    uint8_t previous = 0;
    bool in_frame = false;
    size_t block_bytes;
    while ((block_bytes = fread(s_index_buffer, 1, sizeof(s_index_buffer), file)) != 0) {
        for (size_t index = 0; index < block_bytes; ++index) {
            const uint8_t current = s_index_buffer[index];
            if (!in_frame && previous == 0xFF && current == 0xD8) {
                start = position - 1U;
                in_frame = true;
            } else if (in_frame && previous == 0xFF && current == 0xD9) {
                if (s_media.frame_count < M5_MAX_FRAMES) {
                    const uint32_t end = position + 1U;
                    const uint32_t length = end - start;
                    s_media.frames[s_media.frame_count++] = (m5_frame_index_t) {
                        .offset = start, .length = length,
                    };
                    if (length > s_media.largest_frame) s_media.largest_frame = length;
                } else {
                    ESP_LOGW(TAG, "M5_MEDIA result=too_many_frames limit=%u", M5_MAX_FRAMES);
                    fclose(file);
                    s_media_index_error = 4; return false;
                }
                in_frame = false;
            }
            previous = current;
            ++position;
        }
    }
    fclose(file);
    if (in_frame || s_media.frame_count == 0) { s_media_index_error = 3; return false; }

    s_media.file_size = (uint32_t) size;
    s_media.file = fopen(M5_JPEG_PATH, "rb");
    if (s_media.file == NULL) { s_media_index_error = 5; return false; }

    const size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const size_t preload_need = (size_t) s_media.file_size + M5_RGB565_BYTES + M5_MAX_JPEG_BYTES;
    if (s_media.file_size <= M5_PRELOAD_LIMIT && free_psram > preload_need) {
        s_media.preload = heap_caps_malloc(s_media.file_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_media.preload != NULL && fread(s_media.preload, 1, s_media.file_size, s_media.file) == s_media.file_size) {
            s_media.preloaded = true;
        } else {
            heap_caps_free(s_media.preload);
            s_media.preload = NULL;
        }
        fseek(s_media.file, 0, SEEK_SET);
    }
    if (!s_media.preloaded) {
        const uint32_t buffer_size = s_media.largest_frame > M5_MAX_JPEG_BYTES ? 0 : s_media.largest_frame;
        if (buffer_size == 0) {
            ESP_LOGW(TAG, "M5_MEDIA source=sd result=frame_too_large bytes=%u", s_media.largest_frame);
            fclose(s_media.file);
            s_media.file = NULL;
            s_media_index_error = 6; return false;
        }
        s_media.read_buffer = heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_media.read_buffer == NULL) {
            fclose(s_media.file);
            s_media.file = NULL;
            s_media_index_error = 7; return false;
        }
    }
    if (s_media.panel_buffers[0] == NULL) {
        ESP_LOGW(TAG, "M5_MEDIA result=frame_buffers_unavailable");
        heap_caps_free(s_media.read_buffer);
        heap_caps_free(s_media.preload);
        s_media.read_buffer = NULL;
        s_media.preload = NULL;
        fclose(s_media.file);
        s_media.file = NULL;
        s_media_index_error = 8;
        return false;
    }
    s_media.ready = true;
    ESP_LOGI(TAG, "M5_MEDIA source=%s frames=%u bytes=%u largest=%u fps=30",
             s_media.preloaded ? "psram" : "sd", s_media.frame_count,
             s_media.file_size, s_media.largest_frame);
    return true;
}

static const uint8_t *m5_frame_bytes(uint32_t index, uint32_t *length)
{
    const m5_frame_index_t *frame = &s_media.frames[index % s_media.frame_count];
    *length = frame->length;
    if (s_media.preloaded) return s_media.preload + frame->offset;
    if (frame->length > s_media.largest_frame || fseek(s_media.file, frame->offset, SEEK_SET) != 0 ||
        fread(s_media.read_buffer, 1, frame->length, s_media.file) != frame->length) {
        return NULL;
    }
    return s_media.read_buffer;
}

static bool m5_decode_and_draw(uint32_t index)
{
    uint32_t input_size = 0;
    const uint8_t *input = m5_frame_bytes(index, &input_size);
    if (input == NULL) return false;
    jpeg_decode_picture_info_t info;
    if (jpeg_decoder_get_info(input, input_size, &info) != ESP_OK ||
        info.width != M5_PANEL_WIDTH || info.height != M5_PANEL_HEIGHT) {
        return false;
    }
    uint32_t output_size = 0;
    const jpeg_decode_cfg_t decode_cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
    };
    if (jpeg_decoder_process(s_media.decoder, &decode_cfg, input, input_size,
                             s_media.panel_buffers[s_media.panel_buffer_index], M5_RGB565_BYTES,
                             &output_size) != ESP_OK ||
        output_size < M5_RGB565_BYTES) {
        return false;
    }
    if (esp_lv_adapter_lock(1000) == ESP_OK) {
        const esp_err_t result = esp_lcd_panel_draw_bitmap(s_panel, 0, 0, M5_PANEL_WIDTH,
                                                            M5_PANEL_HEIGHT,
                                                            s_media.panel_buffers[s_media.panel_buffer_index]);
        s_media.panel_buffer_index = (s_media.panel_buffer_index + 1U) % 3U;
        esp_lv_adapter_unlock();
        return result == ESP_OK;
    }
    return false;
}

static void m5_media_task(void *argument)
{
    (void) argument;
    const bool bundle_ready = m5_load_ui_bundle();
    if (bundle_ready && esp_lv_adapter_lock(1000) == ESP_OK) {
        m5_render_active_ui();
        esp_lv_adapter_refresh_now(s_display);
        esp_lv_adapter_unlock();
    }
    const bool media_ready = m5_index_mjpeg();
    if (!media_ready) {
        ESP_LOGI(TAG, "M5_MEDIA source=none frames=0 fps=30");
    }
    s_last_activity_us = esp_timer_get_time();
    ESP_LOGI(TAG, "M5_COMPLETE animation_fps=15 saver_ready=%u", media_ready);
    uint32_t saver_frame = 0;
    int64_t next_frame_us = 0;
    int64_t next_icon_frame_us = 0;
    while (true) {
        if (!s_ui_ready) {
            /* The adapter lock is not guaranteed to be available from
             * app_main immediately after bsp_display_start. Retry from the
             * task once the LVGL worker owns its normal scheduling loop. */
            if (esp_lv_adapter_lock(1000) == ESP_OK) {
                m5_render_active_ui();
                esp_lv_adapter_unlock();
                s_ui_ready = true;
                ESP_LOGI(TAG, "M5_UI state=ready animation_fps=15");
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        const int64_t now = esp_timer_get_time();
        if (s_screensaver_requested) {
            s_screensaver_requested = false;
            if (s_state == M5_STATE_ACTIVE && s_media.ready && esp_lv_adapter_lock(1000) == ESP_OK) {
                if (!m5_enter_saver_ui()) {
                    esp_lv_adapter_unlock();
                    continue;
                }
                esp_lv_adapter_unlock();
                saver_frame = 0;
                next_frame_us = 0;
                ESP_LOGI(TAG, "M5_STATE from=active to=playing reason=desktop_test");
                continue;
            }
        }
        if (s_wake_requested) {
            if (esp_lv_adapter_lock(1000) == ESP_OK) {
                if (s_media.decoder != NULL) {
                    ESP_ERROR_CHECK(jpeg_del_decoder_engine(s_media.decoder));
                    s_media.decoder = NULL;
                }
                if (s_icon_decoder == NULL) ESP_ERROR_CHECK(esp_lv_decoder_init(&s_icon_decoder));
                m5_render_active_ui();
                esp_lv_adapter_unlock();
                s_wake_requested = false;
                s_state = M5_STATE_ACTIVE;
                s_last_activity_us = esp_timer_get_time();
                ESP_LOGI(TAG, "M5_STATE from=playing to=active reason=touch consumed=1");
            }
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        if (s_state == M5_STATE_ACTIVE) {
            if (s_page_change_requested && esp_lv_adapter_lock(1000) == ESP_OK) {
                m5_render_active_ui();
                esp_lv_adapter_unlock();
                s_page_change_requested = false;
                continue;
            }
            if (s_screensaver_enabled && s_media.ready && now - s_last_activity_us >= (int64_t) s_screensaver_idle_seconds * 1000000) {
                if (esp_lv_adapter_lock(1000) == ESP_OK) {
                    if (!m5_enter_saver_ui()) {
                        esp_lv_adapter_unlock();
                        continue;
                    }
                    esp_lv_adapter_unlock();
                    saver_frame = 0;
                    next_frame_us = 0;
                    continue;
                }
            }
            if (s_visible_animation_count && now >= next_icon_frame_us &&
                now - s_last_activity_us > 100000 && esp_lv_adapter_lock(20) == ESP_OK) {
                const int64_t started = esp_timer_get_time();
                const uint8_t target_fps = m5_animation_target_fps();
                const uint8_t advanced = m5_advance_animation_batch(target_fps);
                esp_lv_adapter_unlock();
                const int64_t elapsed = esp_timer_get_time() - started;
                /* Stagger a busy page instead of invalidating as many as 32
                 * JPEGs in one LVGL cycle. Every icon still advances once per
                 * 15 FPS period, but input only competes with a small batch. */
                next_icon_frame_us = esp_timer_get_time() +
                    ((int64_t) 1000000 * advanced /
                     ((int64_t) target_fps * s_visible_animation_count));
                if (elapsed > 50000) {
                    ESP_LOGW(TAG, "M5_ANIMATION fps_target=%u visible=%u batch=%u cycle_us=%lld overloaded=1",
                             target_fps, s_visible_animation_count, advanced, (long long) elapsed);
                }
            }
            /* Input callbacks run in LVGL's task, but page changes and other
             * deferred UI work are completed here. Keep this interval short
             * and do no redraw work while idle so touch feedback stays crisp. */
            vTaskDelay(pdMS_TO_TICKS(M5_ACTIVE_POLL_MS));
            continue;
        }

        if (s_media.ready && now >= next_frame_us) {
            const bool drawn = m5_decode_and_draw(saver_frame++);
            if (!drawn) {
                ESP_LOGW(TAG, "M5_FRAME index=%u dropped=1", saver_frame - 1);
            }
            const int64_t period_us = 1000000 / M5_SCREENSAVER_FPS;
            if (next_frame_us == 0) next_frame_us = now;
            next_frame_us += period_us;
            /* Drop timeline slots instead of slowing the whole video when a
             * read/decode overruns. Input therefore gets CPU time promptly. */
            if (next_frame_us <= esp_timer_get_time()) {
                const uint32_t skipped = (uint32_t) ((esp_timer_get_time() - next_frame_us) / period_us) + 1U;
                saver_frame += skipped;
                next_frame_us += (int64_t) skipped * period_us;
                ESP_LOGW(TAG, "M5_FRAME index=%u dropped=1 count=%u reason=deadline", saver_frame, skipped);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

void m5_media_start(lv_display_t *display)
{
    s_display = display;
    s_panel = bsp_display_get_panel_handle();
    s_last_activity_us = esp_timer_get_time();
    memset(&s_media, 0, sizeof(s_media));
    s_macro_mutex = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(s_macro_mutex ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(s_panel, 3,
                    &s_media.panel_buffers[0], &s_media.panel_buffers[1],
                    &s_media.panel_buffers[2]));
    s_ui_ready = false;
    s_wake_requested = false;
    s_page_change_requested = false;
    ESP_ERROR_CHECK(esp_lv_decoder_init(&s_icon_decoder));
    /* Paint the first page before touching the potentially large media file.
     * The worker indexes it in the background after the usable UI is visible. */
    if (esp_lv_adapter_lock(1000) == ESP_OK) {
        m5_render_active_ui();
        ESP_ERROR_CHECK(esp_lv_adapter_refresh_now(s_display));
        esp_lv_adapter_unlock();
        s_ui_ready = true;
        ESP_LOGI(TAG, "M5_UI state=ready animation_fps=15");
    } else {
        ESP_LOGE(TAG, "M5_UI result=initial_lock_timeout");
    }
    BaseType_t task_ok = xTaskCreate(m5_media_task, "m5_media", 8192, NULL, 5, NULL);
    ESP_ERROR_CHECK(task_ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    task_ok = xTaskCreate(m5_macro_task, "m5_macro", 4096, NULL, 6, &s_macro_task_handle);
    ESP_ERROR_CHECK(task_ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_LOGI(TAG, "M5_MEDIA indexing=background");
}

uint32_t m5_media_trigger_screensaver(void)
{
    if (!s_media.ready) {
        if (s_media.file != NULL) fclose(s_media.file);
        heap_caps_free(s_media.read_buffer);
        heap_caps_free(s_media.preload);
        s_media.file = NULL;
        s_media.read_buffer = NULL;
        s_media.preload = NULL;
        s_media.preloaded = false;
        s_media.frame_count = 0;
        s_media.largest_frame = 0;
        s_media.file_size = 0;
        if (!m5_index_mjpeg()) return s_media_index_error ? s_media_index_error : UINT32_MAX;
    }
    s_screensaver_requested = true;
    return 0;
}
