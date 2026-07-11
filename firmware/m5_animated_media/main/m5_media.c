/*
 * M5 — bounded animated page and hardware-JPEG screensaver.
 *
 * The M3 transport remains the owner of the card and the USB device.  This
 * module only consumes a fixed, device-local media path after that transport
 * and the shared LVGL display are ready.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "driver/jpeg_decode.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "m5_media.h"

static const char *TAG = "m5";

#define M5_COLUMNS 8
#define M5_ROWS 4
#define M5_BUTTONS (M5_COLUMNS * M5_ROWS)
#define M5_GAP_PX 8
#define M5_MARGIN_X_PX 16
#define M5_ANIMATION_FPS 15
#define M5_ANIMATION_PERIOD_MS (1000 / M5_ANIMATION_FPS)
#define M5_SCREENSAVER_IDLE_MS 15000
#define M5_SCREENSAVER_FPS 30
#define M5_MAX_FRAMES 900
#define M5_PRELOAD_LIMIT (8U * 1024U * 1024U)
#define M5_MAX_JPEG_BYTES (512U * 1024U)
#define M5_JPEG_PATH BSP_SD_MOUNT_POINT "/screendeck/screensaver.mjpg"
#define M5_LCD_WIDTH 1280
#define M5_LCD_HEIGHT 720
#define M5_RGB565_BYTES (M5_LCD_WIDTH * M5_LCD_HEIGHT * 2U)

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
    uint8_t *decoded_buffer;
    jpeg_decoder_handle_t decoder;
    bool ready;
    bool preloaded;
} m5_media_t;

static lv_display_t *s_display;
static esp_lcd_panel_handle_t s_panel;
static lv_obj_t *s_icons[M5_BUTTONS];
static lv_obj_t *s_saver_input;
static m5_media_t s_media;
static volatile m5_state_t s_state = M5_STATE_ACTIVE;
static volatile int64_t s_last_activity_us;
static volatile bool s_ui_ready;
static volatile bool s_wake_requested;

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

static const uint32_t s_colors[M5_BUTTONS] = {
    0x0E7490, 0x1D4ED8, 0x4338CA, 0x7E22CE,
    0xBE123C, 0xC2410C, 0xA16207, 0x4D7C0F,
    0x0F766E, 0x0369A1, 0x6D28D9, 0x9D174D,
    0x111827, 0x111827, 0x111827, 0x166534,
    0x155E75, 0x1E40AF, 0x3730A3, 0x6B21A8,
    0x9F1239, 0x9A3412, 0x854D0E, 0x3F6212,
    0x115E59, 0x075985, 0x5B21B6, 0x831843,
    0x1F2937, 0x1F2937, 0x1F2937, 0x166534,
};

static void m5_touch_activity(void)
{
    s_last_activity_us = esp_timer_get_time();
}

static void m5_render_active_ui(void);

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

static void m5_enter_saver_ui(void)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
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
}

static void m5_start_icon_scheduler(uint32_t tick)
{
    /* A fixed 15 FPS cadence bounds LVGL work even when the page is full. */
    for (uint32_t index = 0; index < M5_BUTTONS; ++index) {
        if (s_icons[index] == NULL) continue;
        const uint32_t phase = (tick + index * 7U) % 30U;
        const uint8_t opa = (phase <= 15U) ? (uint8_t) (LV_OPA_60 + phase * 3U)
                                           : (uint8_t) (LV_OPA_100 - (phase - 15U) * 3U);
        lv_obj_set_style_opa(s_icons[index], opa, 0);
    }
}

static void m5_render_active_ui(void)
{
    if (s_display == NULL) return;
    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
    s_saver_input = NULL;
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    const int width = lv_display_get_horizontal_resolution(s_display);
    const int height = lv_display_get_vertical_resolution(s_display);
    const int tile_size = (width - (2 * M5_MARGIN_X_PX) - ((M5_COLUMNS - 1) * M5_GAP_PX)) / M5_COLUMNS;
    const int grid_height = (M5_ROWS * tile_size) + ((M5_ROWS - 1) * M5_GAP_PX);
    const int top = (height - grid_height) / 2;

    memset(s_icons, 0, sizeof(s_icons));
    for (uint8_t row = 0; row < M5_ROWS; ++row) {
        for (uint8_t column = 0; column < M5_COLUMNS; ++column) {
            const uint8_t index = row * M5_COLUMNS + column;
            lv_obj_t *tile = lv_button_create(screen);
            lv_obj_set_size(tile, tile_size, tile_size);
            lv_obj_set_pos(tile, M5_MARGIN_X_PX + column * (tile_size + M5_GAP_PX),
                           top + row * (tile_size + M5_GAP_PX));
            lv_obj_set_style_radius(tile, 12, 0);
            lv_obj_set_style_border_width(tile, 0, 0);
            lv_obj_set_style_bg_color(tile, lv_color_hex(s_colors[index]), 0);
            lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(tile, lv_color_hex(0xFFFFFF), LV_STATE_PRESSED);
            lv_obj_set_style_bg_opa(tile, LV_OPA_30, LV_STATE_PRESSED);
            lv_obj_add_event_cb(tile, m5_input_event_cb, LV_EVENT_ALL, NULL);
            lv_obj_t *icon = lv_label_create(tile);
            lv_label_set_text(icon, s_symbols[index]);
            lv_obj_set_style_text_color(icon, lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_text_font(icon, &lv_font_montserrat_14, 0);
            lv_obj_center(icon);
            s_icons[index] = icon;
        }
    }
    ESP_LOGI(TAG, "M5_UI grid=8x4 square_px=%d black_bars_px=%d,%d", tile_size, top,
             height - grid_height - top);
}

static bool m5_index_mjpeg(void)
{
    FILE *file = fopen(M5_JPEG_PATH, "rb");
    if (file == NULL) return false;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return false;
    }
    const long size = ftell(file);
    if (size <= 0 || size > UINT32_MAX || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return false;
    }

    uint32_t position = 0;
    uint32_t start = 0;
    uint8_t previous = 0;
    bool in_frame = false;
    int value;
    while ((value = fgetc(file)) != EOF) {
        const uint8_t current = (uint8_t) value;
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
                return false;
            }
            in_frame = false;
        }
        previous = current;
        ++position;
    }
    fclose(file);
    if (in_frame || s_media.frame_count == 0) return false;

    s_media.file_size = (uint32_t) size;
    s_media.file = fopen(M5_JPEG_PATH, "rb");
    if (s_media.file == NULL) return false;

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
            return false;
        }
        s_media.read_buffer = heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_media.read_buffer == NULL) {
            fclose(s_media.file);
            s_media.file = NULL;
            return false;
        }
    }
    s_media.decoded_buffer = heap_caps_malloc(M5_RGB565_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_media.decoded_buffer == NULL || jpeg_new_decoder_engine(&(jpeg_decode_engine_cfg_t) {
        .intr_priority = 1, .timeout_ms = 100,
    }, &s_media.decoder) != ESP_OK) {
        ESP_LOGW(TAG, "M5_MEDIA result=decoder_unavailable");
        heap_caps_free(s_media.decoded_buffer);
        heap_caps_free(s_media.read_buffer);
        heap_caps_free(s_media.preload);
        s_media.decoded_buffer = NULL;
        s_media.read_buffer = NULL;
        s_media.preload = NULL;
        fclose(s_media.file);
        s_media.file = NULL;
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
        info.width != M5_LCD_WIDTH || info.height != M5_LCD_HEIGHT) {
        return false;
    }
    uint32_t output_size = 0;
    const jpeg_decode_cfg_t decode_cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
    };
    if (jpeg_decoder_process(s_media.decoder, &decode_cfg, input, input_size,
                             s_media.decoded_buffer, M5_RGB565_BYTES, &output_size) != ESP_OK ||
        output_size < M5_RGB565_BYTES) {
        return false;
    }
    if (esp_lv_adapter_lock(1000) == ESP_OK) {
        const esp_err_t result = esp_lcd_panel_draw_bitmap(s_panel, 0, 0, M5_LCD_WIDTH,
                                                            M5_LCD_HEIGHT, s_media.decoded_buffer);
        esp_lv_adapter_unlock();
        return result == ESP_OK;
    }
    return false;
}

static void m5_media_task(void *argument)
{
    (void) argument;
    uint32_t animation_tick = 0;
    uint32_t saver_frame = 0;
    int64_t next_frame_us = 0;
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
        if (s_wake_requested) {
            if (esp_lv_adapter_lock(1000) == ESP_OK) {
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
            if (s_media.ready && now - s_last_activity_us >= (int64_t) M5_SCREENSAVER_IDLE_MS * 1000) {
                if (esp_lv_adapter_lock(1000) == ESP_OK) {
                    m5_enter_saver_ui();
                    esp_lv_adapter_unlock();
                    saver_frame = 0;
                    next_frame_us = 0;
                    continue;
                }
            }
            if (esp_lv_adapter_lock(1000) == ESP_OK) {
                m5_start_icon_scheduler(animation_tick++);
                esp_lv_adapter_unlock();
            }
            vTaskDelay(pdMS_TO_TICKS(M5_ANIMATION_PERIOD_MS));
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
    s_ui_ready = false;
    s_wake_requested = false;
    const bool media_ready = m5_index_mjpeg();
    if (!media_ready) {
        ESP_LOGI(TAG, "M5_MEDIA source=none frames=0 fps=30");
    }

    /* Render the first page synchronously. Leaving the initial paint to the
     * worker task made a failed/late worker look like a permanently white
     * panel, even though the LCD and LVGL were correctly initialized. */
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
    ESP_LOGI(TAG, "M5_COMPLETE animation_fps=15 saver_ready=%u", media_ready);
}
