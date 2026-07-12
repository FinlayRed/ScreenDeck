/*
 * M1 touch-to-HID vertical slice for Screendeck.
 *
 * The M1 grid intentionally carries text labels to make the electrical and
 * Windows HID path observable.  The product UI remains icon-only; M4 replaces
 * these labels with square assets sourced from the desktop editor.
 */

#include <stdbool.h>
#include <inttypes.h>
#include <stdint.h>

#include "bsp/esp-bsp.h"
#include "class/hid/hid_device.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"

static const char *TAG = "m1";

#define M1_COLUMNS              8
#define M1_ROWS                 4
#define M1_TILE_GAP_PX          8
#define M1_HORIZONTAL_MARGIN_PX 16
#define M1_KEY_RELEASE_MS       25
#define M1_HID_QUEUE_LENGTH     32
#define M1_HID_REPORT_ID         1
#define M1_CONSUMER_REPORT_ID    2
#define M1_MOUSE_REPORT_ID       3
#define M1_WINUSB_VENDOR_REQUEST 0x21
#define M1_MS_OS_20_DESCRIPTOR_LENGTH 0xB2

typedef struct {
    uint8_t column;
    uint8_t row;
    uint8_t hid_keycode;
} m1_tile_t;

typedef struct {
    uint8_t hid_keycode;
    int64_t touch_release_us;
    uint32_t event_id;
} m1_hid_event_t;

static QueueHandle_t s_hid_queue;
static m1_tile_t s_tiles[M1_COLUMNS * M1_ROWS];

/* A single, standard boot-keyboard report.  F13--F24 are valid usages in the
 * Keyboard/Keypad usage page and do not require a vendor driver on Windows. */
static volatile uint32_t s_touch_event_id;
static volatile uint32_t s_queue_drops;

static const uint8_t s_hid_report_descriptor[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(M1_HID_REPORT_ID)),
    TUD_HID_REPORT_DESC_CONSUMER(HID_REPORT_ID(M1_CONSUMER_REPORT_ID)),
    TUD_HID_REPORT_DESC_MOUSE(HID_REPORT_ID(M1_MOUSE_REPORT_ID)),
};

static const char *s_hid_string_descriptors[] = {
    (const char[]) {0x09, 0x04},
    "Screendeck",
    "Screendeck M1 Touch HID",
    "M1-TOUCH-COMPOSITE",
    "Keyboard, consumer and mouse HID",
    "M1 WinUSB probe",
};

#define M1_TUSB_DESCRIPTOR_LENGTH (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN + TUD_VENDOR_DESC_LEN)

static const tusb_desc_device_t s_device_descriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0210,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303A,
    .idProduct = 0x4010,
    .bcdDevice = 0x0102,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

static const tusb_desc_device_qualifier_t s_device_qualifier = {
    .bLength = sizeof(tusb_desc_device_qualifier_t),
    .bDescriptorType = TUSB_DESC_DEVICE_QUALIFIER,
    .bcdUSB = 0x0210,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .bNumConfigurations = 1,
    .bReserved = 0,
};

static const uint8_t s_hid_configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, 2, 0, M1_TUSB_DESCRIPTOR_LENGTH,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_DESCRIPTOR(0, 4, HID_ITF_PROTOCOL_NONE,
                       sizeof(s_hid_report_descriptor), 0x81, 16, 10),
    TUD_VENDOR_DESCRIPTOR(1, 5, 0x02, 0x82, 64),
};

#if (TUD_OPT_HIGH_SPEED)
static const uint8_t s_hid_high_speed_configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, 2, 0, M1_TUSB_DESCRIPTOR_LENGTH,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_DESCRIPTOR(0, 4, HID_ITF_PROTOCOL_NONE,
                       sizeof(s_hid_report_descriptor), 0x81, 16, 10),
    TUD_VENDOR_DESCRIPTOR(1, 5, 0x02, 0x82, 512),
};
#endif

#define M1_BOS_TOTAL_LENGTH (TUD_BOS_DESC_LEN + TUD_BOS_MICROSOFT_OS_DESC_LEN)
static const uint8_t s_bos_descriptor[] = {
    TUD_BOS_DESCRIPTOR(M1_BOS_TOTAL_LENGTH, 1),
    TUD_BOS_MS_OS_20_DESCRIPTOR(M1_MS_OS_20_DESCRIPTOR_LENGTH, M1_WINUSB_VENDOR_REQUEST),
};

static const uint8_t s_ms_os_20_descriptor[] = {
    U16_TO_U8S_LE(0x000A), U16_TO_U8S_LE(MS_OS_20_SET_HEADER_DESCRIPTOR),
    U32_TO_U8S_LE(0x06030000), U16_TO_U8S_LE(M1_MS_OS_20_DESCRIPTOR_LENGTH),
    U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_CONFIGURATION),
    0, 0, U16_TO_U8S_LE(M1_MS_OS_20_DESCRIPTOR_LENGTH - 0x0A),
    U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_FUNCTION),
    1, 0, U16_TO_U8S_LE(M1_MS_OS_20_DESCRIPTOR_LENGTH - 0x0A - 0x08),
    U16_TO_U8S_LE(0x0014), U16_TO_U8S_LE(MS_OS_20_FEATURE_COMPATBLE_ID),
    'W', 'I', 'N', 'U', 'S', 'B', 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    U16_TO_U8S_LE(M1_MS_OS_20_DESCRIPTOR_LENGTH - 0x0A - 0x08 - 0x08 - 0x14),
    U16_TO_U8S_LE(MS_OS_20_FEATURE_REG_PROPERTY),
    U16_TO_U8S_LE(0x0007), U16_TO_U8S_LE(0x002A),
    'D', 0, 'e', 0, 'v', 0, 'i', 0, 'c', 0, 'e', 0,
    'I', 0, 'n', 0, 't', 0, 'e', 0, 'r', 0, 'f', 0,
    'a', 0, 'c', 0, 'e', 0, 'G', 0, 'U', 0, 'I', 0,
    'D', 0, 's', 0, 0, 0,
    U16_TO_U8S_LE(0x0050),
    '{', 0, 'F', 0, '3', 0, '8', 0, 'C', 0, '2', 0,
    '5', 0, '3', 0, 'C', 0, '-', 0, '7', 0, 'E', 0,
    '9', 0, '5', 0, '-', 0, '4', 0, 'F', 0, '1', 0,
    '5', 0, '-', 0, 'A', 0, '9', 0, 'F', 0, 'D', 0,
    '-', 0, '7', 0, 'B', 0, 'B', 0, 'C', 0, '3', 0,
    '1', 0, 'E', 0, '4', 0, 'F', 0, '0', 0, 'C', 0,
    '3', 0, '}', 0, 0, 0, 0, 0,
};

TU_VERIFY_STATIC(sizeof(s_ms_os_20_descriptor) == M1_MS_OS_20_DESCRIPTOR_LENGTH,
                 "M1 MS OS 2.0 descriptor length mismatch");

uint8_t const *tud_descriptor_bos_cb(void) { return s_bos_descriptor; }

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                tusb_control_request_t const *request)
{
    if (stage != CONTROL_STAGE_SETUP) return true;
    if (request->bmRequestType_bit.type != TUSB_REQ_TYPE_VENDOR ||
        request->bRequest != M1_WINUSB_VENDOR_REQUEST || request->wIndex != 7) return false;
    return tud_control_xfer(rhport, request, (void *) s_ms_os_20_descriptor,
                            sizeof(s_ms_os_20_descriptor));
}

void tud_vendor_rx_cb(uint8_t itf, uint8_t const *buffer, uint16_t bufsize)
{
    (void) itf; (void) buffer;
    ESP_LOGI(TAG, "M1_WINUSB result=rx_discarded bytes=%u", bufsize);
    tud_vendor_n_read_flush(0);
}

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void) instance;
    return s_hid_report_descriptor;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type, uint8_t *buffer,
                               uint16_t reqlen)
{
    (void) instance;
    (void) report_id;
    (void) report_type;
    (void) buffer;
    (void) reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize)
{
    (void) instance;
    (void) report_id;
    (void) report_type;
    (void) buffer;
    (void) bufsize;
}

static void usb_event_cb(tinyusb_event_t *event, void *argument)
{
    (void) argument;
    if (event->id == TINYUSB_EVENT_ATTACHED) {
        ESP_LOGI(TAG, "M1_USB state=mounted interfaces=hid_keyboard_consumer_mouse,winusb");
    } else if (event->id == TINYUSB_EVENT_DETACHED) {
        ESP_LOGW(TAG, "M1_USB state=unmounted");
    }
}

static const char *hid_usage_name(uint8_t hid_keycode)
{
    static const char *const names[] = {
        "F13", "F14", "F15", "F16", "F17", "F18",
        "F19", "F20", "F21", "F22", "F23", "F24",
    };
    return names[hid_keycode - HID_KEY_F13];
}

static void hid_sender_task(void *argument)
{
    (void) argument;
    m1_hid_event_t hid_event = {0};

    while (true) {
        if (xQueueReceive(s_hid_queue, &hid_event, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (!tud_mounted() || !tud_hid_n_ready(0)) {
            ESP_LOGW(TAG, "M1_HID result=not_ready key=%s mounted=%u",
                     hid_usage_name(hid_event.hid_keycode), tud_mounted());
            continue;
        }

        uint8_t keycodes[6] = {hid_event.hid_keycode};
        if (!tud_hid_keyboard_report(M1_HID_REPORT_ID, 0, keycodes)) {
            ESP_LOGW(TAG, "M1_HID result=press_send_failed key=%s event=%u", hid_usage_name(hid_event.hid_keycode), hid_event.event_id);
            continue;
        }

        const int64_t submitted_us = esp_timer_get_time();
        ESP_LOGI(TAG, "M1_HID action=press key=%s event=%u touch_to_submit_us=%" PRId64 " target_us=10000 pass=%u queue_drops=%u",
                 hid_usage_name(hid_event.hid_keycode), hid_event.event_id,
                 submitted_us - hid_event.touch_release_us,
                 submitted_us - hid_event.touch_release_us <= 10000, s_queue_drops);
        vTaskDelay(pdMS_TO_TICKS(M1_KEY_RELEASE_MS));

        for (int retry = 0; retry < 20 && !tud_hid_n_ready(0); ++retry) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        if (tud_hid_keyboard_report(M1_HID_REPORT_ID, 0, NULL)) {
            ESP_LOGI(TAG, "M1_HID action=release key=%s duration_ms=%u",
                     hid_usage_name(hid_event.hid_keycode), M1_KEY_RELEASE_MS);
        } else {
            ESP_LOGW(TAG, "M1_HID result=release_send_failed key=%s", hid_usage_name(hid_event.hid_keycode));
        }
    }
}

static void tile_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    const m1_tile_t *tile = lv_event_get_user_data(event);
    lv_indev_t *indev = lv_indev_get_act();
    lv_point_t point = {0};
    if (indev != NULL) {
        lv_indev_get_point(indev, &point);
    }

    const m1_hid_event_t hid_event = {
        .hid_keycode = tile->hid_keycode,
        .touch_release_us = esp_timer_get_time(),
        .event_id = ++s_touch_event_id,
    };
    const BaseType_t queued = xQueueSend(s_hid_queue, &hid_event, 0);
    if (queued != pdTRUE) ++s_queue_drops;
    ESP_LOGI(TAG, "M1_TOUCH event=%u release_us=%" PRId64 " logical_x=%d logical_y=%d column=%u row=%u key=%s queued=%u queue_drops=%u",
             hid_event.event_id, hid_event.touch_release_us,
             point.x, point.y, tile->column, tile->row,
             hid_usage_name(tile->hid_keycode), queued == pdTRUE, s_queue_drops);
}

static void create_square_macro_grid(lv_display_t *display)
{
    static const uint32_t colors[] = {
        0x0E7490, 0x1D4ED8, 0x4338CA, 0x7E22CE,
        0xBE123C, 0xC2410C, 0xA16207, 0x4D7C0F,
        0x0F766E, 0x0369A1, 0x6D28D9, 0x9D174D,
    };

    lv_obj_t *screen = lv_screen_active();
    const int width = lv_display_get_horizontal_resolution(display);
    const int height = lv_display_get_vertical_resolution(display);
    const int tile_size = (width - (2 * M1_HORIZONTAL_MARGIN_PX) -
                           ((M1_COLUMNS - 1) * M1_TILE_GAP_PX)) / M1_COLUMNS;
    const int grid_height = (M1_ROWS * tile_size) + ((M1_ROWS - 1) * M1_TILE_GAP_PX);
    const int top_bar_height = (height - grid_height) / 2;

    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    for (uint8_t row = 0; row < M1_ROWS; ++row) {
        for (uint8_t column = 0; column < M1_COLUMNS; ++column) {
            const uint8_t index = row * M1_COLUMNS + column;
            const uint8_t hid_keycode = HID_KEY_F13 + (index % 12);
            s_tiles[index] = (m1_tile_t) {
                .column = column,
                .row = row,
                .hid_keycode = hid_keycode,
            };

            lv_obj_t *tile = lv_button_create(screen);
            lv_obj_set_size(tile, tile_size, tile_size);
            lv_obj_set_pos(tile,
                           M1_HORIZONTAL_MARGIN_PX + column * (tile_size + M1_TILE_GAP_PX),
                           top_bar_height + row * (tile_size + M1_TILE_GAP_PX));
            lv_obj_set_style_radius(tile, 12, 0);
            lv_obj_set_style_border_width(tile, 0, 0);
            lv_obj_set_style_bg_color(tile,
                                      lv_color_hex(colors[index % (sizeof(colors) / sizeof(colors[0]))]), 0);
            lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(tile, lv_color_hex(0xFFFFFF), LV_STATE_PRESSED);
            lv_obj_set_style_bg_opa(tile, LV_OPA_30, LV_STATE_PRESSED);
            lv_obj_add_event_cb(tile, tile_event_cb, LV_EVENT_CLICKED, &s_tiles[index]);

            lv_obj_t *label = lv_label_create(tile);
            lv_label_set_text(label, hid_usage_name(hid_keycode));
            lv_obj_center(label);
        }
    }

    ESP_LOGI(TAG,
             "M1_DISPLAY logical_width=%d logical_height=%d grid=%dx%d tile_px=%d top_black_bar_px=%d bottom_black_bar_px=%d rotation=90",
             width, height, M1_COLUMNS, M1_ROWS, tile_size, top_bar_height,
             height - grid_height - top_bar_height);
}

static void init_usb_keyboard(void)
{
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    tusb_cfg.descriptor.device = &s_device_descriptor;
    tusb_cfg.descriptor.qualifier = &s_device_qualifier;
    tusb_cfg.descriptor.full_speed_config = s_hid_configuration_descriptor;
    tusb_cfg.descriptor.string = s_hid_string_descriptors;
    tusb_cfg.descriptor.string_count = sizeof(s_hid_string_descriptors) / sizeof(s_hid_string_descriptors[0]);
    tusb_cfg.event_cb = usb_event_cb;
#if (TUD_OPT_HIGH_SPEED)
    tusb_cfg.descriptor.high_speed_config = s_hid_high_speed_configuration_descriptor;
#endif
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));
    ESP_LOGI(TAG, "M1_USB state=ready vid=303A pid=4010 hid_reports=keyboard,consumer,mouse winusb=1 keys=F13-F24");
}

void app_main(void)
{
    ESP_LOGI(TAG, "M1_START idf=%s target=esp32p4", esp_get_idf_version());

    s_hid_queue = xQueueCreate(M1_HID_QUEUE_LENGTH, sizeof(m1_hid_event_t));
    ESP_ERROR_CHECK(s_hid_queue == NULL ? ESP_ERR_NO_MEM : ESP_OK);
    BaseType_t task_created = xTaskCreate(hid_sender_task, "m1_hid", 4096, NULL, 5, NULL);
    ESP_ERROR_CHECK(task_created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    init_usb_keyboard();

    bsp_display_cfg_t display_cfg = {
        .lv_adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG(),
        .rotation = ESP_LV_ADAPTER_ROTATE_90,
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_PARTIAL,
        .touch_flags = {
            // The GT911 is physically 720x1280 portrait. ROTATE_90 renders
            // the LCD as 1280x720 landscape. With the board mounted USB-right
            // the required transform is (x, y) -> (1279 - y, x).
            .swap_xy = true,
            .mirror_x = true,
            .mirror_y = false,
        },
    };

    lv_display_t *display = bsp_display_start_with_config(&display_cfg);
    if (display == NULL) {
        ESP_LOGE(TAG, "M1_DISPLAY result=start_failed");
        return;
    }
    bsp_display_backlight_on();

    /* Do not use bsp_display_lock(): the vendor wrapper is declared bool but
     * returns esp_err_t.  The LVGL adapter lock is type-safe. */
    ESP_ERROR_CHECK(esp_lv_adapter_lock(UINT32_MAX));
    create_square_macro_grid(display);
    esp_lv_adapter_unlock();

    ESP_LOGI(TAG, "M1_COMPLETE grid_ready=1 composite_hid=1 winusb=1 latency_telemetry=1 usb_waiting_for_otg_host=1");
}
