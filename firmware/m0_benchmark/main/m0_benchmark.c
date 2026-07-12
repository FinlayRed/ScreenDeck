/*
 * M0 hardware benchmark for the Screendeck ESP32-P4 macro pad.
 *
 * This is intentionally a diagnostic program, not product firmware.  It
 * validates the board path and produces repeatable serial measurements before
 * the UI, HID, and media architecture is committed.
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bsp/esp-bsp.h"
#include "driver/jpeg_decode.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

static const char *TAG = "m0";

#define M0_COLUMNS                 8
#define M0_ROWS                    4
#define M0_PSRAM_COPY_BYTES        (4 * 1024 * 1024)
#define M0_PSRAM_COPY_ITERATIONS   24
#define M0_SD_IO_BYTES             (8 * 1024 * 1024)
#define M0_SD_BLOCK_BYTES          (128 * 1024)
#define M0_JPEG_ITERATIONS         30
#define M0_DISPLAY_ITERATIONS      30
#define M0_FRAME_WIDTH             1280
#define M0_FRAME_HEIGHT            720
#define M0_FRAME_BYTES             (M0_FRAME_WIDTH * M0_FRAME_HEIGHT * 2)
#define M0_TARGET_FRAME_US         33333

extern const uint8_t _binary_m0_frame_jpg_start[];
extern const uint8_t _binary_m0_frame_jpg_end[];

typedef struct {
    uint8_t column;
    uint8_t row;
} m0_tile_t;

static m0_tile_t s_tiles[M0_COLUMNS * M0_ROWS];
static lv_display_t *s_display;
static lv_obj_t *s_media_image;
static lv_image_dsc_t s_media_descriptor;
static volatile bool s_combined_active;
static volatile uint32_t s_combined_touch_events;
static volatile int64_t s_combined_start_us;

static esp_lv_adapter_tear_avoid_mode_t selected_tear_mode(void)
{
#if CONFIG_M0_TEAR_DOUBLE_DIRECT
    return ESP_LV_ADAPTER_TEAR_AVOID_MODE_DOUBLE_DIRECT;
#elif CONFIG_M0_TEAR_DOUBLE_FULL
    return ESP_LV_ADAPTER_TEAR_AVOID_MODE_DOUBLE_FULL;
#else
    return ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_PARTIAL;
#endif
}

static const char *selected_tear_mode_name(void)
{
#if CONFIG_M0_TEAR_DOUBLE_DIRECT
    return "double_direct";
#elif CONFIG_M0_TEAR_DOUBLE_FULL
    return "double_full";
#else
    return "triple_partial";
#endif
}

static void log_heap(const char *checkpoint)
{
    const size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const size_t psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    const size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    ESP_LOGI(TAG,
             "M0_HEAP checkpoint=%s psram_total=%u psram_free=%u psram_largest=%u internal_free=%u internal_largest=%u",
             checkpoint,
             (unsigned int) esp_psram_get_size(),
             (unsigned int) psram_free,
             (unsigned int) psram_largest,
             (unsigned int) internal_free,
             (unsigned int) internal_largest);
}

static void tile_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_PRESSED) {
        return;
    }

    const m0_tile_t *tile = lv_event_get_user_data(event);
    lv_indev_t *indev = lv_indev_get_act();
    lv_point_t point = {0};
    if (indev != NULL) {
        lv_indev_get_point(indev, &point);
    }

    ESP_LOGI(TAG, "M0_TOUCH logical_x=%d logical_y=%d column=%u row=%u",
             point.x, point.y, tile->column, tile->row);
    if (s_combined_active) {
        ++s_combined_touch_events;
        ESP_LOGI(TAG,
                 "M0_TOUCH_WORKLOAD case=combined event=%u elapsed_us=%" PRId64 " timestamp_source=lvgl_event_callback latency_us=unavailable",
                 (unsigned int) s_combined_touch_events,
                 esp_timer_get_time() - s_combined_start_us);
    }
}

static void create_touch_grid(lv_display_t *display)
{
    static const uint32_t colors[] = {
        0x0E7490, 0x1D4ED8, 0x4338CA, 0x7E22CE,
        0xBE123C, 0xC2410C, 0xA16207, 0x4D7C0F,
    };

    lv_obj_t *screen = lv_screen_active();
    const int width = lv_display_get_horizontal_resolution(display);
    const int height = lv_display_get_vertical_resolution(display);
    const int gap = 8;
    const int tile_width = (width - gap * (M0_COLUMNS + 1)) / M0_COLUMNS;
    const int tile_height = (height - gap * (M0_ROWS + 1)) / M0_ROWS;

    lv_obj_set_style_bg_color(screen, lv_color_hex(0x080B12), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    for (uint8_t row = 0; row < M0_ROWS; ++row) {
        for (uint8_t column = 0; column < M0_COLUMNS; ++column) {
            const uint8_t index = row * M0_COLUMNS + column;
            s_tiles[index] = (m0_tile_t) {.column = column, .row = row};

            lv_obj_t *tile = lv_button_create(screen);
            lv_obj_set_size(tile, tile_width, tile_height);
            lv_obj_set_pos(tile, gap + column * (tile_width + gap), gap + row * (tile_height + gap));
            lv_obj_set_style_radius(tile, 10, 0);
            lv_obj_set_style_border_width(tile, 0, 0);
            lv_obj_set_style_bg_color(tile, lv_color_hex(colors[index % (sizeof(colors) / sizeof(colors[0]))]), 0);
            lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(tile, lv_color_hex(0xFFFFFF), LV_STATE_PRESSED);
            lv_obj_set_style_bg_opa(tile, LV_OPA_30, LV_STATE_PRESSED);
            lv_obj_add_event_cb(tile, tile_event_cb, LV_EVENT_PRESSED, &s_tiles[index]);

            lv_obj_t *label = lv_label_create(tile);
            lv_label_set_text_fmt(label, "%u,%u", column + 1, row + 1);
            lv_obj_center(label);
        }
    }

    ESP_LOGI(TAG, "M0_DISPLAY logical_width=%d logical_height=%d grid=%dx%d rotation=90", width, height, M0_COLUMNS, M0_ROWS);

    s_media_image = lv_image_create(screen);
    lv_obj_set_size(s_media_image, width, height);
    lv_obj_set_pos(s_media_image, 0, 0);
    lv_obj_add_flag(s_media_image, LV_OBJ_FLAG_HIDDEN);
}

static void benchmark_psram_copy(void)
{
    uint8_t *source = heap_caps_malloc(M0_PSRAM_COPY_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *destination = heap_caps_malloc(M0_PSRAM_COPY_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (source == NULL || destination == NULL) {
        ESP_LOGE(TAG, "M0_PSRAM result=allocation_failed bytes=%u", M0_PSRAM_COPY_BYTES);
        free(source);
        free(destination);
        return;
    }

    for (size_t i = 0; i < M0_PSRAM_COPY_BYTES; ++i) {
        source[i] = (uint8_t) (i * 37U + 11U);
    }

    const int64_t start_us = esp_timer_get_time();
    for (int iteration = 0; iteration < M0_PSRAM_COPY_ITERATIONS; ++iteration) {
        memcpy(destination, source, M0_PSRAM_COPY_BYTES);
    }
    const int64_t elapsed_us = esp_timer_get_time() - start_us;

    volatile uint32_t checksum = 0;
    for (size_t i = 0; i < M0_PSRAM_COPY_BYTES; i += 4096) {
        checksum += destination[i];
    }

    const double mib_per_second = ((double) M0_PSRAM_COPY_BYTES * M0_PSRAM_COPY_ITERATIONS * 1000000.0) /
                                  ((double) elapsed_us * 1024.0 * 1024.0);
    ESP_LOGI(TAG, "M0_PSRAM bytes=%u iterations=%u elapsed_us=%" PRId64 " throughput_mib_s=%.2f checksum=%u",
             M0_PSRAM_COPY_BYTES, M0_PSRAM_COPY_ITERATIONS, elapsed_us, mib_per_second, (unsigned int) checksum);

    free(source);
    free(destination);
}

typedef struct {
    jpeg_decoder_handle_t engine;
    uint8_t *input;
    size_t input_capacity;
    uint8_t *output;
    size_t output_capacity;
    jpeg_decode_picture_info_t picture;
} m0_decoder_t;

static void decoder_deinit(m0_decoder_t *decoder)
{
    if (decoder->engine != NULL) {
        jpeg_del_decoder_engine(decoder->engine);
    }
    free(decoder->output);
    free(decoder->input);
    memset(decoder, 0, sizeof(*decoder));
}

static esp_err_t decoder_init(m0_decoder_t *decoder, size_t encoded_capacity)
{
    memset(decoder, 0, sizeof(*decoder));
    jpeg_decode_memory_alloc_cfg_t input_cfg = {.buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER};
    jpeg_decode_memory_alloc_cfg_t output_cfg = {.buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER};
    decoder->input = jpeg_alloc_decoder_mem(encoded_capacity, &input_cfg, &decoder->input_capacity);
    if (decoder->input == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const size_t embedded_size = _binary_m0_frame_jpg_end - _binary_m0_frame_jpg_start;
    memcpy(decoder->input, _binary_m0_frame_jpg_start, embedded_size);
    esp_err_t result = jpeg_decoder_get_info(decoder->input, embedded_size, &decoder->picture);
    if (result != ESP_OK || decoder->picture.width != M0_FRAME_WIDTH || decoder->picture.height != M0_FRAME_HEIGHT) {
        decoder_deinit(decoder);
        return result == ESP_OK ? ESP_ERR_INVALID_SIZE : result;
    }

    const size_t aligned_width = ((size_t) decoder->picture.width + 15U) & ~(size_t) 15U;
    const size_t aligned_height = ((size_t) decoder->picture.height + 15U) & ~(size_t) 15U;
    decoder->output = jpeg_alloc_decoder_mem(aligned_width * aligned_height * 2, &output_cfg, &decoder->output_capacity);
    if (decoder->output == NULL) {
        decoder_deinit(decoder);
        return ESP_ERR_NO_MEM;
    }

    const jpeg_decode_engine_cfg_t engine_cfg = {.timeout_ms = 1000};
    result = jpeg_new_decoder_engine(&engine_cfg, &decoder->engine);
    if (result != ESP_OK) {
        decoder_deinit(decoder);
    }
    return result;
}

static esp_err_t decoder_process(m0_decoder_t *decoder, size_t encoded_size, uint32_t *output_size)
{
    const jpeg_decode_cfg_t cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
    };
    return jpeg_decoder_process(decoder->engine, &cfg, decoder->input, encoded_size,
                                decoder->output, decoder->output_capacity, output_size);
}

static void benchmark_sd_io(void)
{
    const esp_err_t mount_result = bsp_sdcard_mount();
    if (mount_result != ESP_OK) {
        ESP_LOGE(TAG, "M0_SD result=mount_failed error=%s", esp_err_to_name(mount_result));
        return;
    }

    uint8_t *buffer = heap_caps_malloc(M0_SD_BLOCK_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    uint8_t *stdio_buffer = malloc(M0_SD_BLOCK_BYTES);
    if (buffer == NULL || stdio_buffer == NULL) {
        ESP_LOGE(TAG, "M0_SD result=buffer_allocation_failed bytes=%u", M0_SD_BLOCK_BYTES);
        free(stdio_buffer);
        free(buffer);
        bsp_sdcard_unmount();
        return;
    }

    for (size_t i = 0; i < M0_SD_BLOCK_BYTES; ++i) {
        buffer[i] = (uint8_t) (i * 29U + 7U);
    }

    char path[96];
    snprintf(path, sizeof(path), "%s/m0_sd_io.bin", BSP_SD_MOUNT_POINT);
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        ESP_LOGE(TAG, "M0_SD result=open_write_failed path=%s", path);
        free(stdio_buffer);
        free(buffer);
        bsp_sdcard_unmount();
        return;
    }
    setvbuf(file, (char *) stdio_buffer, _IOFBF, M0_SD_BLOCK_BYTES);

    int64_t start_us = esp_timer_get_time();
    size_t total = 0;
    while (total < M0_SD_IO_BYTES) {
        const size_t to_write = (M0_SD_IO_BYTES - total) < M0_SD_BLOCK_BYTES ? (M0_SD_IO_BYTES - total) : M0_SD_BLOCK_BYTES;
        if (fwrite(buffer, 1, to_write, file) != to_write) {
            ESP_LOGE(TAG, "M0_SD result=write_failed offset=%u", (unsigned int) total);
            fclose(file);
            unlink(path);
            free(stdio_buffer);
            free(buffer);
            bsp_sdcard_unmount();
            return;
        }
        total += to_write;
    }
    fflush(file);
    fsync(fileno(file));
    fclose(file);
    const int64_t write_elapsed_us = esp_timer_get_time() - start_us;

    memset(buffer, 0, M0_SD_BLOCK_BYTES);
    file = fopen(path, "rb");
    if (file == NULL) {
        ESP_LOGE(TAG, "M0_SD result=open_read_failed path=%s", path);
        unlink(path);
        free(stdio_buffer);
        free(buffer);
        bsp_sdcard_unmount();
        return;
    }
    setvbuf(file, (char *) stdio_buffer, _IOFBF, M0_SD_BLOCK_BYTES);

    int64_t read_elapsed_us[2] = {0};
    uint32_t checksum[2] = {0};
    for (int pass = 0; pass < 2; ++pass) {
        rewind(file);
        clearerr(file);
        start_us = esp_timer_get_time();
        total = 0;
        while (total < M0_SD_IO_BYTES) {
            const size_t to_read = (M0_SD_IO_BYTES - total) < M0_SD_BLOCK_BYTES ? (M0_SD_IO_BYTES - total) : M0_SD_BLOCK_BYTES;
            if (fread(buffer, 1, to_read, file) != to_read) {
                ESP_LOGE(TAG, "M0_SD result=read_failed pass=%d offset=%u", pass + 1, (unsigned int) total);
                fclose(file);
                unlink(path);
                free(stdio_buffer);
                free(buffer);
                bsp_sdcard_unmount();
                return;
            }
            for (size_t i = 0; i < to_read; i += 4096) {
                checksum[pass] += buffer[i];
            }
            total += to_read;
        }
        read_elapsed_us[pass] = esp_timer_get_time() - start_us;
        const double pass_mib_s = ((double) M0_SD_IO_BYTES * 1000000.0) /
                                  ((double) read_elapsed_us[pass] * 1024.0 * 1024.0);
        ESP_LOGI(TAG,
                 "M0_SD_READ case=buffered_sequential pass=%d bytes=%u application_block_bytes=%u stdio_buffer_bytes=%u elapsed_us=%" PRId64 " throughput_mib_s=%.2f checksum=%u",
                 pass + 1, M0_SD_IO_BYTES, M0_SD_BLOCK_BYTES, M0_SD_BLOCK_BYTES,
                 read_elapsed_us[pass], pass_mib_s, (unsigned int) checksum[pass]);
    }
    fclose(file);
    const bool temp_removed = unlink(path) == 0;

    const double write_mib_s = ((double) M0_SD_IO_BYTES * 1000000.0) / ((double) write_elapsed_us * 1024.0 * 1024.0);
    const double read_mib_s = ((double) M0_SD_IO_BYTES * 1000000.0) / ((double) read_elapsed_us[0] * 1024.0 * 1024.0);
    ESP_LOGI(TAG, "M0_SD result=%s bytes=%u write_us=%" PRId64 " write_mib_s=%.2f first_read_us=%" PRId64 " first_read_mib_s=%.2f destructive=0 temp_removed=%d",
             temp_removed ? "ok" : "cleanup_failed", M0_SD_IO_BYTES, write_elapsed_us, write_mib_s,
             read_elapsed_us[0], read_mib_s, temp_removed ? 1 : 0);

    free(stdio_buffer);
    free(buffer);
    bsp_sdcard_unmount();
}

static void benchmark_jpeg_decoder(void)
{
    const size_t jpeg_size = _binary_m0_frame_jpg_end - _binary_m0_frame_jpg_start;
    m0_decoder_t decoder;
    esp_err_t result = decoder_init(&decoder, jpeg_size);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "M0_JPEG result=init_failed error=%s", esp_err_to_name(result));
        return;
    }
    uint32_t output_size = 0;
    result = decoder_process(&decoder, jpeg_size, &output_size);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "M0_JPEG result=warmup_failed error=%s", esp_err_to_name(result));
        decoder_deinit(&decoder);
        return;
    }

    const int64_t start_us = esp_timer_get_time();
    for (int iteration = 0; iteration < M0_JPEG_ITERATIONS; ++iteration) {
        result = decoder_process(&decoder, jpeg_size, &output_size);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "M0_JPEG result=decode_failed iteration=%d error=%s", iteration, esp_err_to_name(result));
            decoder_deinit(&decoder);
            return;
        }
    }
    const int64_t elapsed_us = esp_timer_get_time() - start_us;
    const double fps = ((double) M0_JPEG_ITERATIONS * 1000000.0) / (double) elapsed_us;
    ESP_LOGI(TAG, "M0_JPEG result=ok case=decode_only width=%" PRId32 " height=%" PRId32 " encoded_bytes=%u output_bytes=%u iterations=%u elapsed_us=%" PRId64 " achieved_fps=%.2f timing_includes=jpeg_hardware_process_only",
             decoder.picture.width, decoder.picture.height, (unsigned int) jpeg_size, output_size, M0_JPEG_ITERATIONS, elapsed_us, fps);
    decoder_deinit(&decoder);
}

static void benchmark_display_handoff(void)
{
    uint8_t *frame = heap_caps_malloc(M0_FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (frame == NULL) {
        ESP_LOGE(TAG, "M0_DISPLAY_CASE result=allocation_failed bytes=%u", M0_FRAME_BYTES);
        return;
    }
    memset(frame, 0x5a, M0_FRAME_BYTES);

    esp_err_t result = esp_lv_adapter_set_dummy_draw(s_display, true);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "M0_DISPLAY_CASE result=dummy_enable_failed error=%s", esp_err_to_name(result));
        free(frame);
        return;
    }
    const int64_t start_us = esp_timer_get_time();
    uint32_t failed = 0;
    for (uint32_t i = 0; i < M0_DISPLAY_ITERATIONS; ++i) {
        frame[0] = (uint8_t) i;
        result = esp_lv_adapter_dummy_draw_blit(s_display, 0, 0, 720, 1280, frame, true);
        if (result != ESP_OK) {
            ++failed;
        }
    }
    const int64_t elapsed_us = esp_timer_get_time() - start_us;
    (void) esp_lv_adapter_set_dummy_draw(s_display, false);
    const double fps = failed == M0_DISPLAY_ITERATIONS ? 0.0 :
                       ((double) (M0_DISPLAY_ITERATIONS - failed) * 1000000.0) / (double) elapsed_us;
    ESP_LOGI(TAG,
             "M0_DISPLAY_CASE result=%s case=direct_full_frame_wait tear_mode=%s panel_width=720 panel_height=1280 bytes_per_frame=%u frames=%u failed_frames=%u elapsed_us=%" PRId64 " achieved_fps=%.2f timing_includes=adapter_blit_submit_and_color_transfer_done timing_excludes=decode,lvgl_render,scanout_to_photons",
             failed == 0 ? "ok" : "failed", selected_tear_mode_name(), M0_FRAME_BYTES,
             M0_DISPLAY_ITERATIONS, failed, elapsed_us, fps);
    free(frame);
}

static esp_err_t write_combined_stream(const char *path, uint32_t frames)
{
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        return ESP_FAIL;
    }
    const size_t jpeg_size = _binary_m0_frame_jpg_end - _binary_m0_frame_jpg_start;
    for (uint32_t i = 0; i < frames; ++i) {
        if (fwrite(_binary_m0_frame_jpg_start, 1, jpeg_size, file) != jpeg_size) {
            fclose(file);
            unlink(path);
            return ESP_FAIL;
        }
    }
    fflush(file);
    fsync(fileno(file));
    fclose(file);
    return ESP_OK;
}

static void benchmark_combined(void)
{
    const size_t jpeg_size = _binary_m0_frame_jpg_end - _binary_m0_frame_jpg_start;
    const uint32_t requested_frames = CONFIG_M0_COMBINED_FRAMES;
    const esp_err_t mount_result = bsp_sdcard_mount();
    if (mount_result != ESP_OK) {
        ESP_LOGE(TAG, "M0_COMBINED result=mount_failed error=%s", esp_err_to_name(mount_result));
        return;
    }
    char path[96];
    snprintf(path, sizeof(path), "%s/m0_media_stream.mjpg", BSP_SD_MOUNT_POINT);
    if (write_combined_stream(path, requested_frames) != ESP_OK) {
        ESP_LOGE(TAG, "M0_COMBINED result=stream_create_failed temp_removed=1");
        bsp_sdcard_unmount();
        return;
    }

    m0_decoder_t decoder;
    esp_err_t result = decoder_init(&decoder, jpeg_size);
    FILE *file = NULL;
    uint8_t *stdio_buffer = NULL;
    if (result == ESP_OK) {
        file = fopen(path, "rb");
        stdio_buffer = malloc(M0_SD_BLOCK_BYTES);
    }
    if (result != ESP_OK || file == NULL || stdio_buffer == NULL) {
        ESP_LOGE(TAG, "M0_COMBINED result=setup_failed error=%s", esp_err_to_name(result));
        if (file != NULL) fclose(file);
        free(stdio_buffer);
        decoder_deinit(&decoder);
        unlink(path);
        bsp_sdcard_unmount();
        return;
    }
    setvbuf(file, (char *) stdio_buffer, _IOFBF, M0_SD_BLOCK_BYTES);

    memset(&s_media_descriptor, 0, sizeof(s_media_descriptor));
    s_media_descriptor.header.cf = LV_COLOR_FORMAT_RGB565;
    s_media_descriptor.header.w = M0_FRAME_WIDTH;
    s_media_descriptor.header.h = M0_FRAME_HEIGHT;
    s_media_descriptor.header.stride = M0_FRAME_WIDTH * 2;
    s_media_descriptor.data_size = M0_FRAME_BYTES;
    s_media_descriptor.data = decoder.output;

    int64_t read_us = 0;
    int64_t decode_us = 0;
    int64_t display_us = 0;
    int64_t max_frame_us = 0;
    int64_t max_lvgl_lock_us = 0;
    uint32_t completed = 0;
    uint32_t failed = 0;
    uint32_t dropped = 0;
    uint32_t output_size = 0;
    s_combined_touch_events = 0;
    s_combined_start_us = esp_timer_get_time();
    s_combined_active = true;

    for (uint32_t frame = 0; frame < requested_frames; ++frame) {
        const int64_t frame_start = esp_timer_get_time();
        int64_t stage_start = frame_start;
        if (fread(decoder.input, 1, jpeg_size, file) != jpeg_size) {
            ++failed;
            break;
        }
        read_us += esp_timer_get_time() - stage_start;

        stage_start = esp_timer_get_time();
        result = decoder_process(&decoder, jpeg_size, &output_size);
        decode_us += esp_timer_get_time() - stage_start;
        if (result != ESP_OK || output_size < M0_FRAME_BYTES) {
            ++failed;
            continue;
        }

        stage_start = esp_timer_get_time();
        const int64_t lock_start = stage_start;
        result = esp_lv_adapter_lock(1000);
        const int64_t lock_us = esp_timer_get_time() - lock_start;
        if (lock_us > max_lvgl_lock_us) max_lvgl_lock_us = lock_us;
        if (result == ESP_OK) {
            lv_image_set_src(s_media_image, &s_media_descriptor);
            lv_obj_remove_flag(s_media_image, LV_OBJ_FLAG_HIDDEN);
            lv_obj_invalidate(s_media_image);
            esp_lv_adapter_unlock();
            result = esp_lv_adapter_refresh_now(s_display);
        }
        display_us += esp_timer_get_time() - stage_start;
        if (result != ESP_OK) {
            ++failed;
            continue;
        }
        ++completed;
        const int64_t frame_us = esp_timer_get_time() - frame_start;
        if (frame_us > max_frame_us) max_frame_us = frame_us;
        if (frame_us > M0_TARGET_FRAME_US) ++dropped;
        const int64_t remaining_us = M0_TARGET_FRAME_US - frame_us;
        if (remaining_us > 0) {
            vTaskDelay(pdMS_TO_TICKS((uint32_t) remaining_us / 1000U));
        }
    }
    s_combined_active = false;
    const int64_t elapsed_us = esp_timer_get_time() - s_combined_start_us;

    if (esp_lv_adapter_lock(1000) == ESP_OK) {
        lv_obj_add_flag(s_media_image, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(lv_screen_active());
        esp_lv_adapter_unlock();
        (void) esp_lv_adapter_refresh_now(s_display);
    }
    fclose(file);
    free(stdio_buffer);
    decoder_deinit(&decoder);
    const bool temp_removed = unlink(path) == 0;
    bsp_sdcard_unmount();

    const double achieved_fps = elapsed_us > 0 ? ((double) completed * 1000000.0) / (double) elapsed_us : 0.0;
    ESP_LOGI(TAG,
             "M0_COMBINED result=%s tear_mode=%s requested_frames=%u completed_frames=%u failed_frames=%u dropped_frames=%u target_frame_us=%u elapsed_us=%" PRId64 " achieved_fps=%.2f sd_read_us=%" PRId64 " jpeg_decode_us=%" PRId64 " lvgl_display_us=%" PRId64 " max_frame_us=%" PRId64 " max_lvgl_lock_us=%" PRId64 " touch_events=%u touch_latency_us=unavailable psram_free=%u psram_largest=%u internal_free=%u temp_removed=%d",
             failed == 0 && temp_removed ? "ok" : "failed", selected_tear_mode_name(), requested_frames, completed,
             failed, dropped, M0_TARGET_FRAME_US, elapsed_us, achieved_fps, read_us, decode_us,
             display_us, max_frame_us, max_lvgl_lock_us, (unsigned int) s_combined_touch_events,
             (unsigned int) heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned int) heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
             (unsigned int) heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT), temp_removed ? 1 : 0);
}

static void benchmark_task(void *argument)
{
    (void) argument;
    vTaskDelay(pdMS_TO_TICKS(1500));

    log_heap("boot");
    benchmark_psram_copy();
    log_heap("after_psram_copy");
    benchmark_jpeg_decoder();
    log_heap("after_jpeg");
    benchmark_display_handoff();
    log_heap("after_display_handoff");
    benchmark_sd_io();
    log_heap("after_sd");
    benchmark_combined();
    log_heap("after_combined");

    ESP_LOGI(TAG, "M0_COMPLETE touch_grid_ready=1 tear_mode=%s serial_markers=M0_DISPLAY,M0_TOUCH,M0_PSRAM,M0_JPEG,M0_DISPLAY_CASE,M0_SD_READ,M0_SD,M0_COMBINED,M0_HEAP", selected_tear_mode_name());
    while (true) {
        log_heap("idle");
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "M0_START idf=%s target=esp32p4", esp_get_idf_version());
    log_heap("pre_display");

    bsp_display_cfg_t display_cfg = {
        .lv_adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG(),
        .rotation = ESP_LV_ADAPTER_ROTATE_90,
        .tear_avoid_mode = selected_tear_mode(),
        .touch_flags = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
    };

    s_display = bsp_display_start_with_config(&display_cfg);
    if (s_display == NULL) {
        ESP_LOGE(TAG, "M0_DISPLAY result=start_failed");
        return;
    }
    bsp_display_backlight_on();

    // The vendor BSP declares this wrapper as bool but returns esp_err_t,
    // making a successful ESP_OK appear false. Follow the vendor examples and
    // ignore its return value for M0; product code will use esp_lv_adapter_lock
    // directly so status remains type-safe.
    (void) bsp_display_lock(UINT32_MAX);
    create_touch_grid(s_display);
    bsp_display_unlock();

    ESP_LOGI(TAG,
             "M0_DISPLAY_CONFIG interface=mipi_dsi tear_mode=%s rotation=90 dpi_buffers=%d mode_comparison=separate_firmware_runs",
             selected_tear_mode_name(), CONFIG_BSP_LCD_DPI_BUFFER_NUMS);

    xTaskCreate(benchmark_task, "m0_bench", 8192, NULL, 4, NULL);
}
