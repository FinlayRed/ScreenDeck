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
#define M0_SD_IO_BYTES             (4 * 1024 * 1024)
#define M0_SD_BLOCK_BYTES          (64 * 1024)
#define M0_JPEG_ITERATIONS         30

extern const uint8_t _binary_esp1080_jpg_start[];
extern const uint8_t _binary_esp1080_jpg_end[];

typedef struct {
    uint8_t column;
    uint8_t row;
} m0_tile_t;

static m0_tile_t s_tiles[M0_COLUMNS * M0_ROWS];

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

static void benchmark_sd_io(void)
{
    const esp_err_t mount_result = bsp_sdcard_mount();
    if (mount_result != ESP_OK) {
        ESP_LOGE(TAG, "M0_SD result=mount_failed error=%s", esp_err_to_name(mount_result));
        return;
    }

    uint8_t *buffer = heap_caps_malloc(M0_SD_BLOCK_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (buffer == NULL) {
        ESP_LOGE(TAG, "M0_SD result=buffer_allocation_failed bytes=%u", M0_SD_BLOCK_BYTES);
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
        free(buffer);
        bsp_sdcard_unmount();
        return;
    }

    int64_t start_us = esp_timer_get_time();
    size_t total = 0;
    while (total < M0_SD_IO_BYTES) {
        const size_t to_write = (M0_SD_IO_BYTES - total) < M0_SD_BLOCK_BYTES ? (M0_SD_IO_BYTES - total) : M0_SD_BLOCK_BYTES;
        if (fwrite(buffer, 1, to_write, file) != to_write) {
            ESP_LOGE(TAG, "M0_SD result=write_failed offset=%u", (unsigned int) total);
            fclose(file);
            unlink(path);
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
        free(buffer);
        bsp_sdcard_unmount();
        return;
    }

    start_us = esp_timer_get_time();
    total = 0;
    uint32_t checksum = 0;
    while (total < M0_SD_IO_BYTES) {
        const size_t to_read = (M0_SD_IO_BYTES - total) < M0_SD_BLOCK_BYTES ? (M0_SD_IO_BYTES - total) : M0_SD_BLOCK_BYTES;
        if (fread(buffer, 1, to_read, file) != to_read) {
            ESP_LOGE(TAG, "M0_SD result=read_failed offset=%u", (unsigned int) total);
            fclose(file);
            unlink(path);
            free(buffer);
            bsp_sdcard_unmount();
            return;
        }
        for (size_t i = 0; i < to_read; i += 4096) {
            checksum += buffer[i];
        }
        total += to_read;
    }
    fclose(file);
    const int64_t read_elapsed_us = esp_timer_get_time() - start_us;
    unlink(path);

    const double write_mib_s = ((double) M0_SD_IO_BYTES * 1000000.0) / ((double) write_elapsed_us * 1024.0 * 1024.0);
    const double read_mib_s = ((double) M0_SD_IO_BYTES * 1000000.0) / ((double) read_elapsed_us * 1024.0 * 1024.0);
    ESP_LOGI(TAG, "M0_SD bytes=%u write_us=%" PRId64 " write_mib_s=%.2f read_us=%" PRId64 " read_mib_s=%.2f checksum=%u",
             M0_SD_IO_BYTES, write_elapsed_us, write_mib_s, read_elapsed_us, read_mib_s, (unsigned int) checksum);

    free(buffer);
    bsp_sdcard_unmount();
}

static void benchmark_jpeg_decoder(void)
{
    const size_t jpeg_size = _binary_esp1080_jpg_end - _binary_esp1080_jpg_start;
    jpeg_decode_memory_alloc_cfg_t input_alloc_cfg = {.buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER};
    jpeg_decode_memory_alloc_cfg_t output_alloc_cfg = {.buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER};
    size_t input_allocated = 0;
    uint8_t *input = jpeg_alloc_decoder_mem(jpeg_size, &input_alloc_cfg, &input_allocated);
    if (input == NULL) {
        ESP_LOGE(TAG, "M0_JPEG result=input_allocation_failed bytes=%u", (unsigned int) jpeg_size);
        return;
    }
    memcpy(input, _binary_esp1080_jpg_start, jpeg_size);

    jpeg_decode_picture_info_t picture = {0};
    esp_err_t result = jpeg_decoder_get_info(input, jpeg_size, &picture);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "M0_JPEG result=header_failed error=%s", esp_err_to_name(result));
        free(input);
        return;
    }

    // The P4 JPEG DMA engine rounds image dimensions to 16-pixel MCU blocks.
    // Allocate against that physical output size, not the visible dimensions.
    const size_t aligned_width = ((size_t) picture.width + 15U) & ~(size_t) 15U;
    const size_t aligned_height = ((size_t) picture.height + 15U) & ~(size_t) 15U;
    const size_t requested_output_size = aligned_width * aligned_height * 2;
    size_t output_allocated = 0;
    uint8_t *output = jpeg_alloc_decoder_mem(requested_output_size, &output_alloc_cfg, &output_allocated);
    if (output == NULL) {
        ESP_LOGE(TAG, "M0_JPEG result=output_allocation_failed bytes=%u", (unsigned int) requested_output_size);
        free(input);
        return;
    }

    jpeg_decoder_handle_t decoder = NULL;
    const jpeg_decode_engine_cfg_t decoder_cfg = {.timeout_ms = 1000};
    result = jpeg_new_decoder_engine(&decoder_cfg, &decoder);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "M0_JPEG result=engine_create_failed error=%s", esp_err_to_name(result));
        free(output);
        free(input);
        return;
    }

    const jpeg_decode_cfg_t decode_cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
    };
    uint32_t output_size = 0;
    result = jpeg_decoder_process(decoder, &decode_cfg, input, jpeg_size, output, output_allocated, &output_size);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "M0_JPEG result=warmup_failed error=%s", esp_err_to_name(result));
        jpeg_del_decoder_engine(decoder);
        free(output);
        free(input);
        return;
    }

    const int64_t start_us = esp_timer_get_time();
    for (int iteration = 0; iteration < M0_JPEG_ITERATIONS; ++iteration) {
        result = jpeg_decoder_process(decoder, &decode_cfg, input, jpeg_size, output, output_allocated, &output_size);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "M0_JPEG result=decode_failed iteration=%d error=%s", iteration, esp_err_to_name(result));
            jpeg_del_decoder_engine(decoder);
            free(output);
            free(input);
            return;
        }
    }
    const int64_t elapsed_us = esp_timer_get_time() - start_us;
    const double fps = ((double) M0_JPEG_ITERATIONS * 1000000.0) / (double) elapsed_us;
    ESP_LOGI(TAG, "M0_JPEG width=%" PRId32 " height=%" PRId32 " encoded_bytes=%u output_bytes=%u iterations=%u elapsed_us=%" PRId64 " fps=%.2f",
             picture.width, picture.height, (unsigned int) jpeg_size, output_size, M0_JPEG_ITERATIONS, elapsed_us, fps);

    jpeg_del_decoder_engine(decoder);
    free(output);
    free(input);
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
    benchmark_sd_io();
    log_heap("after_sd");

    ESP_LOGI(TAG, "M0_COMPLETE touch_grid_ready=1 serial_markers=M0_DISPLAY,M0_TOUCH,M0_PSRAM,M0_JPEG,M0_SD");
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
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_PARTIAL,
        .touch_flags = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
    };

    lv_display_t *display = bsp_display_start_with_config(&display_cfg);
    if (display == NULL) {
        ESP_LOGE(TAG, "M0_DISPLAY result=start_failed");
        return;
    }
    bsp_display_backlight_on();

    // The vendor BSP declares this wrapper as bool but returns esp_err_t,
    // making a successful ESP_OK appear false. Follow the vendor examples and
    // ignore its return value for M0; product code will use esp_lv_adapter_lock
    // directly so status remains type-safe.
    (void) bsp_display_lock(UINT32_MAX);
    create_touch_grid(display);
    bsp_display_unlock();

    xTaskCreate(benchmark_task, "m0_bench", 8192, NULL, 4, NULL);
}
