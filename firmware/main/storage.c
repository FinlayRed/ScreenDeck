// SPDX-License-Identifier: GPL-3.0-or-later

/*
 * M3 — USB bundle sync and microSD ownership.
 *
 * The P4 owns the card at all times.  The vendor endpoint transports framed
 * sync messages; it never exposes a mass-storage interface to Windows.
 */

#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bsp/esp-bsp.h"
#include "class/hid/hid_device.h"
#include "esp_crc.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"

#ifdef M5_MEDIA_ENABLED
#include "m6_media.h"
#endif

static const char *TAG = "m3";

#define M3_PROTOCOL_MAGIC 0x33434453UL /* SDC3 */
#define M3_PROTOCOL_VERSION 1
#define M3_BUNDLE_MAGIC 0x33424453UL   /* SDB3 */
#define M3_POINTER_MAGIC 0x33525450UL  /* PTR3 */
#define M3_MAX_FRAME_PAYLOAD 1400
#define M3_RX_FRAME_BYTES (sizeof(m3_frame_header_t) + M3_MAX_FRAME_PAYLOAD)
#define M3_RX_PACKET_BYTES CONFIG_TINYUSB_VENDOR_RX_BUFSIZE
#define M3_RX_QUEUE_DEPTH 24
#define M3_ROOT BSP_SD_MOUNT_POINT "/screendeck"
#define M3_BUNDLES_DIR M3_ROOT "/bundles"
#define M3_STAGE_FILE M3_ROOT "/upload.part"
#define M3_STATE_FILE M3_ROOT "/upload.state"
#define M3_MEDIA_STAGE_FILE M3_ROOT "/screensaver.upload"
#define M3_MEDIA_FILE M3_ROOT "/screensaver.mjpg"
#define M3_MEDIA_BACKUP_FILE M3_ROOT "/screensaver.previous"
#define M3_MAX_BUNDLE_BYTES (16U * 1024U * 1024U)
#define M3_MAX_MEDIA_BYTES (16U * 1024U * 1024U)
#define M3_BUNDLE_WRITE_BUFFER_BYTES (64U * 1024U)
#define M3_MEDIA_WRITE_BUFFER_BYTES (64U * 1024U)
#define M3_RESPONSE_WAIT_MS 250
#define M3_FRAME_FLAG_NO_RESPONSE 0x0001U
#define M3_HID_REPORT_ID 1
#define M3_CONSUMER_REPORT_ID 2
#define M3_MOUSE_REPORT_ID 3
#define M3_WINUSB_VENDOR_REQUEST 0x21
#define M3_MS_OS_20_DESCRIPTOR_LENGTH 0xB2

typedef enum {
    M3_OP_HELLO = 1,
    M3_OP_BEGIN = 2,
    M3_OP_CHUNK = 3,
    M3_OP_COMMIT = 4,
    M3_OP_ABORT = 5,
    M3_OP_STATUS = 6,
    M3_OP_DIAG = 7,
    M3_OP_MEDIA_BEGIN = 8,
    M3_OP_MEDIA_CHUNK = 9,
    M3_OP_MEDIA_COMMIT = 10,
    M3_OP_MEDIA_ABORT = 11,
    M3_OP_TEST_SCREENSAVER = 12,
    M3_OP_DOWNLOAD_BEGIN = 13,
    M3_OP_DOWNLOAD_CHUNK = 14,
} m3_opcode_t;

typedef enum {
    M3_STATUS_OK = 0,
    M3_STATUS_BAD_FRAME = 1,
    M3_STATUS_BAD_STATE = 2,
    M3_STATUS_IO = 3,
    M3_STATUS_BAD_BUNDLE = 4,
    M3_STATUS_BUSY = 5,
    M3_STATUS_MEDIA_UNAVAILABLE = 6,
} m3_status_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t version;
    uint8_t opcode;
    uint16_t reserved;
    uint32_t sequence;
    uint32_t payload_size;
    uint32_t payload_crc32;
} m3_frame_header_t;

typedef struct __attribute__((packed)) {
    uint32_t total_bytes;
    uint32_t bundle_crc32;
} m3_begin_t;

typedef struct __attribute__((packed)) {
    uint32_t offset;
    uint32_t chunk_crc32;
} m3_chunk_prefix_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t schema_version;
    uint16_t header_bytes;
    uint32_t total_bytes;
    uint32_t payload_crc32;
} m3_bundle_header_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t generation;
    uint32_t bundle_crc32;
    char bundle_name[32];
    uint32_t record_crc32;
} m3_pointer_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t total_bytes;
    uint32_t bundle_crc32;
    uint32_t received_bytes;
    uint32_t record_crc32;
} m3_upload_state_t;

typedef struct {
    uint16_t length;
    uint8_t bytes[M3_RX_PACKET_BYTES];
} m3_rx_packet_t;

typedef struct {
    bool mounted;
    bool active_bundle_valid;
    bool upload_open;
    uint32_t total_bytes;
    uint32_t bundle_crc32;
    uint32_t received_bytes;
    uint32_t durable_bytes;
    uint32_t active_generation;
    uint32_t previous_generation;
    FILE *upload_file;
    uint8_t *upload_buffer;
    size_t upload_buffer_used;
} m3_storage_state_t;

typedef struct {
    bool open;
    FILE *file;
    uint8_t *write_buffer;
    size_t write_buffer_used;
    uint32_t total_bytes;
    uint32_t crc32;
    uint32_t received_bytes;
} m3_media_upload_t;

static QueueHandle_t s_rx_queue;
static m3_storage_state_t s_storage;
static m3_media_upload_t s_media_upload;
static uint8_t s_frame_buffer[M3_RX_FRAME_BYTES];
static size_t s_frame_length;
static bool s_usb_mounted;
static lv_display_t *s_display;
static char s_active_bundle_path[128];
static FILE *s_download_file;
static uint32_t s_download_offset;

const char *m3_active_bundle_path(void)
{
    return s_active_bundle_path[0] != '\0' ? s_active_bundle_path : NULL;
}

#ifdef M5_MEDIA_ENABLED
static void m3_restart_after_commit(void *argument)
{
    (void) argument;
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}
#endif

static const uint8_t s_hid_report_descriptor[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(M3_HID_REPORT_ID)),
    TUD_HID_REPORT_DESC_CONSUMER(HID_REPORT_ID(M3_CONSUMER_REPORT_ID)),
    TUD_HID_REPORT_DESC_MOUSE(HID_REPORT_ID(M3_MOUSE_REPORT_ID)),
};

static const char *s_string_descriptors[] = {
    (const char[]) {0x09, 0x04}, "Screendeck", "Screendeck M3 Sync",
    "M3-SYNC-STORAGE", "Keyboard", "Sync channel",
};

#define M3_TUSB_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN + TUD_VENDOR_DESC_LEN)

static const tusb_desc_device_t s_device_descriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    // USB 2.1 is required for Windows to query the BOS MS-OS 2.0 descriptor.
    .bcdUSB = 0x0210,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303A,
    // PID 4005 was flashed before the WinUSB BOS descriptor existed. Use a
    // fresh pre-release PID so Windows performs a clean driver selection.
    .idProduct = 0x4011,
    .bcdDevice = 0x0101,
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

static const uint8_t s_full_speed_configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, 2, 0, M3_TUSB_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_DESCRIPTOR(0, 4, HID_ITF_PROTOCOL_NONE,
                       sizeof(s_hid_report_descriptor), 0x81, 16, 10),
    TUD_VENDOR_DESCRIPTOR(1, 5, 0x02, 0x82, 64),
};

#if (TUD_OPT_HIGH_SPEED)
static const uint8_t s_high_speed_configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, 2, 0, M3_TUSB_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_DESCRIPTOR(0, 4, HID_ITF_PROTOCOL_NONE,
                       sizeof(s_hid_report_descriptor), 0x81, 16, 10),
    TUD_VENDOR_DESCRIPTOR(1, 5, 0x02, 0x82, 512),
};
#endif

#define M3_BOS_TOTAL_LENGTH (TUD_BOS_DESC_LEN + TUD_BOS_MICROSOFT_OS_DESC_LEN)

static const uint8_t s_bos_descriptor[] = {
    TUD_BOS_DESCRIPTOR(M3_BOS_TOTAL_LENGTH, 1),
    TUD_BOS_MS_OS_20_DESCRIPTOR(M3_MS_OS_20_DESCRIPTOR_LENGTH, M3_WINUSB_VENDOR_REQUEST),
};

static const uint8_t s_ms_os_20_descriptor[] = {
    // Set header: length, type, Windows version, total length.
    U16_TO_U8S_LE(0x000A), U16_TO_U8S_LE(MS_OS_20_SET_HEADER_DESCRIPTOR),
    U32_TO_U8S_LE(0x06030000), U16_TO_U8S_LE(M3_MS_OS_20_DESCRIPTOR_LENGTH),
    // Configuration subset header.
    U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_CONFIGURATION),
    0, 0, U16_TO_U8S_LE(M3_MS_OS_20_DESCRIPTOR_LENGTH - 0x0A),
    // Function subset header. Interface 1 is the vendor sync endpoint.
    U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_FUNCTION),
    1, 0, U16_TO_U8S_LE(M3_MS_OS_20_DESCRIPTOR_LENGTH - 0x0A - 0x08),
    // Compatible ID descriptor: instruct Windows to bind WinUSB.
    U16_TO_U8S_LE(0x0014), U16_TO_U8S_LE(MS_OS_20_FEATURE_COMPATBLE_ID),
    'W', 'I', 'N', 'U', 'S', 'B', 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    // Registry property descriptor. A stable interface GUID lets the desktop
    // app find this WinUSB function without depending on a mutable device path.
    U16_TO_U8S_LE(M3_MS_OS_20_DESCRIPTOR_LENGTH - 0x0A - 0x08 - 0x08 - 0x14),
    U16_TO_U8S_LE(MS_OS_20_FEATURE_REG_PROPERTY),
    U16_TO_U8S_LE(0x0007), U16_TO_U8S_LE(0x002A),
    'D', 0x00, 'e', 0x00, 'v', 0x00, 'i', 0x00, 'c', 0x00, 'e', 0x00,
    'I', 0x00, 'n', 0x00, 't', 0x00, 'e', 0x00, 'r', 0x00, 'f', 0x00,
    'a', 0x00, 'c', 0x00, 'e', 0x00, 'G', 0x00, 'U', 0x00, 'I', 0x00,
    'D', 0x00, 's', 0x00, 0x00, 0x00,
    U16_TO_U8S_LE(0x0050),
    // {F38C253C-7E95-4F15-A9FD-7BBC31E4F0C4}, followed by a UTF-16 terminator.
    '{', 0x00, 'F', 0x00, '3', 0x00, '8', 0x00, 'C', 0x00, '2', 0x00,
    '5', 0x00, '3', 0x00, 'C', 0x00, '-', 0x00, '7', 0x00, 'E', 0x00,
    '9', 0x00, '5', 0x00, '-', 0x00, '4', 0x00, 'F', 0x00, '1', 0x00,
    '5', 0x00, '-', 0x00, 'A', 0x00, '9', 0x00, 'F', 0x00, 'D', 0x00,
    '-', 0x00, '7', 0x00, 'B', 0x00, 'B', 0x00, 'C', 0x00, '3', 0x00,
    '1', 0x00, 'E', 0x00, '4', 0x00, 'F', 0x00, '0', 0x00, 'C', 0x00,
    '4', 0x00, '}', 0x00, 0x00, 0x00, 0x00, 0x00,
};

TU_VERIFY_STATIC(sizeof(s_ms_os_20_descriptor) == M3_MS_OS_20_DESCRIPTOR_LENGTH,
                 "M3 MS OS 2.0 descriptor length mismatch");

uint8_t const *tud_descriptor_bos_cb(void)
{
    return s_bos_descriptor;
}

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                tusb_control_request_t const *request)
{
    // tud_control_xfer() drives DATA/ACK back through this callback. Once the
    // SETUP stage was accepted, those completion stages must succeed.
    if (stage != CONTROL_STAGE_SETUP) {
        return true;
    }
    if (request->bmRequestType_bit.type != TUSB_REQ_TYPE_VENDOR ||
        request->bRequest != M3_WINUSB_VENDOR_REQUEST || request->wIndex != 7) {
        return false;
    }
    return tud_control_xfer(rhport, request, (void *) s_ms_os_20_descriptor,
                            sizeof(s_ms_os_20_descriptor));
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
    (void) instance; (void) report_id; (void) report_type; (void) buffer; (void) reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize)
{
    (void) instance; (void) report_id; (void) report_type; (void) buffer; (void) bufsize;
}

static uint32_t m3_crc32(const void *data, size_t length)
{
    return esp_crc32_le(UINT32_MAX, data, length);
}

static bool m3_mkdir(const char *path)
{
    if (mkdir(path, 0775) == 0) {
        return true;
    }
    struct stat status;
    return stat(path, &status) == 0 && S_ISDIR(status.st_mode);
}

static bool m3_write_exact(const char *path, const void *data, size_t length)
{
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        return false;
    }
    const bool ok = fwrite(data, 1, length, file) == length && fflush(file) == 0 && fsync(fileno(file)) == 0;
    fclose(file);
    return ok;
}

static bool m3_read_exact(const char *path, void *data, size_t length)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return false;
    }
    const bool ok = fread(data, 1, length, file) == length && fgetc(file) == EOF;
    fclose(file);
    return ok;
}

static bool m3_upload_state_write(void)
{
    m3_upload_state_t record = {
        .magic = M3_PROTOCOL_MAGIC,
        .total_bytes = s_storage.total_bytes,
        .bundle_crc32 = s_storage.bundle_crc32,
        .received_bytes = s_storage.received_bytes,
    };
    record.record_crc32 = m3_crc32(&record, offsetof(m3_upload_state_t, record_crc32));
    return m3_write_exact(M3_STATE_FILE, &record, sizeof(record));
}

static void m3_bundle_upload_close(void)
{
    if (s_storage.upload_file != NULL) fclose(s_storage.upload_file);
    free(s_storage.upload_buffer);
    s_storage.upload_file = NULL;
    s_storage.upload_buffer = NULL;
    s_storage.upload_buffer_used = 0;
}

static bool m3_bundle_upload_open(bool resume)
{
    if (s_storage.upload_file != NULL && s_storage.upload_buffer != NULL) return true;
    FILE *file = fopen(M3_STAGE_FILE, resume ? "ab" : "wb");
    uint8_t *buffer = malloc(M3_BUNDLE_WRITE_BUFFER_BYTES);
    if (file == NULL || buffer == NULL) {
        if (file != NULL) fclose(file);
        free(buffer);
        return false;
    }
    s_storage.upload_file = file;
    s_storage.upload_buffer = buffer;
    s_storage.upload_buffer_used = 0;
    return true;
}

static bool m3_bundle_upload_flush_buffer(void)
{
    if (s_storage.upload_file == NULL || s_storage.upload_buffer == NULL) return false;
    if (s_storage.upload_buffer_used != 0 &&
        fwrite(s_storage.upload_buffer, 1, s_storage.upload_buffer_used,
               s_storage.upload_file) != s_storage.upload_buffer_used) {
        return false;
    }
    s_storage.upload_buffer_used = 0;
    return true;
}

static bool m3_bundle_upload_checkpoint(void)
{
    if (!m3_bundle_upload_flush_buffer() || fflush(s_storage.upload_file) != 0 ||
        fsync(fileno(s_storage.upload_file)) != 0 || !m3_upload_state_write()) {
        return false;
    }
    s_storage.durable_bytes = s_storage.received_bytes;
    return true;
}

static bool m3_upload_state_load(void)
{
    m3_upload_state_t record = {0};
    if (!m3_read_exact(M3_STATE_FILE, &record, sizeof(record)) ||
        record.magic != M3_PROTOCOL_MAGIC ||
        record.record_crc32 != m3_crc32(&record, offsetof(m3_upload_state_t, record_crc32)) ||
        record.total_bytes == 0 || record.total_bytes > M3_MAX_BUNDLE_BYTES ||
        record.received_bytes > record.total_bytes) {
        return false;
    }
    struct stat stage;
    if (stat(M3_STAGE_FILE, &stage) != 0 || (uint32_t) stage.st_size != record.received_bytes) {
        return false;
    }
    s_storage.upload_open = true;
    s_storage.total_bytes = record.total_bytes;
    s_storage.bundle_crc32 = record.bundle_crc32;
    s_storage.received_bytes = record.received_bytes;
    s_storage.durable_bytes = record.received_bytes;
    return true;
}

static bool m3_validate_bundle_file(const char *path, uint32_t expected_bytes, uint32_t expected_crc)
{
    m3_bundle_header_t header = {0};
    FILE *file = fopen(path, "rb");
    if (file == NULL || fread(&header, 1, sizeof(header), file) != sizeof(header) ||
        header.magic != M3_BUNDLE_MAGIC || header.schema_version != M3_PROTOCOL_VERSION ||
        header.header_bytes != sizeof(header) || header.total_bytes != expected_bytes) {
        if (file) fclose(file);
        return false;
    }
    uint8_t block[512];
    uint32_t crc = UINT32_MAX;
    size_t read;
    while ((read = fread(block, 1, sizeof(block), file)) != 0) {
        crc = esp_crc32_le(crc, block, read);
    }
    fclose(file);
    return crc == expected_crc && header.payload_crc32 == expected_crc;
}

static bool m3_read_pointer(const char *path, m3_pointer_t *pointer)
{
    return m3_read_exact(path, pointer, sizeof(*pointer)) &&
           pointer->magic == M3_POINTER_MAGIC &&
           pointer->record_crc32 == m3_crc32(pointer, offsetof(m3_pointer_t, record_crc32)) &&
           memchr(pointer->bundle_name, '\0', sizeof(pointer->bundle_name)) != NULL;
}

static bool m3_parse_generation(const char *name, const char *prefix,
                                const char *suffix, uint32_t *generation)
{
    const size_t prefix_length = strlen(prefix);
    if (strncmp(name, prefix, prefix_length) != 0) return false;
    char *end = NULL;
    const unsigned long value = strtoul(name + prefix_length, &end, 10);
    if (end == name + prefix_length || strcmp(end, suffix) != 0 || value > UINT32_MAX) return false;
    *generation = (uint32_t) value;
    return true;
}

static bool m3_find_pointer_below(uint32_t limit, m3_pointer_t *selected,
                                  char *selected_path, size_t selected_path_size)
{
    bool found = false;
    uint32_t best = 0;
    DIR *directory = opendir(M3_ROOT);
    if (directory == NULL) return false;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        uint32_t filename_generation;
        if (!m3_parse_generation(entry->d_name, "active-", ".ptr", &filename_generation)) continue;
        char pointer_path[384];
        snprintf(pointer_path, sizeof(pointer_path), "%s/%s", M3_ROOT, entry->d_name);
        m3_pointer_t pointer = {0};
        if (!m3_read_pointer(pointer_path, &pointer) || pointer.generation != filename_generation ||
            pointer.generation >= limit || (found && pointer.generation <= best)) continue;
        *selected = pointer;
        snprintf(selected_path, selected_path_size, "%s", pointer_path);
        best = pointer.generation;
        found = true;
    }
    closedir(directory);
    return found;
}

static void m3_cleanup_generations(void)
{
    DIR *directory = opendir(M3_ROOT);
    if (directory != NULL) {
        struct dirent *entry;
        while ((entry = readdir(directory)) != NULL) {
            uint32_t generation;
            if (!m3_parse_generation(entry->d_name, "active-", ".ptr", &generation) ||
                generation == s_storage.active_generation ||
                generation == s_storage.previous_generation) continue;
            char path[384];
            snprintf(path, sizeof(path), "%s/%s", M3_ROOT, entry->d_name);
            unlink(path);
        }
        closedir(directory);
    }
    directory = opendir(M3_BUNDLES_DIR);
    if (directory != NULL) {
        struct dirent *entry;
        while ((entry = readdir(directory)) != NULL) {
            uint32_t generation;
            if (!m3_parse_generation(entry->d_name, "bundle-", ".sdb", &generation) ||
                generation == s_storage.active_generation ||
                generation == s_storage.previous_generation) continue;
            char path[384];
            snprintf(path, sizeof(path), "%s/%s", M3_BUNDLES_DIR, entry->d_name);
            unlink(path);
        }
        closedir(directory);
    }
}

static void m3_find_active_pointer(void)
{
    s_storage.active_bundle_valid = false;
    s_storage.active_generation = 0;
    s_storage.previous_generation = 0;
    s_active_bundle_path[0] = '\0';
    uint32_t limit = UINT32_MAX;
    uint8_t valid_count = 0;
    for (;;) {
        m3_pointer_t pointer = {0};
        char pointer_path[384];
        if (!m3_find_pointer_below(limit, &pointer, pointer_path, sizeof(pointer_path))) break;
        limit = pointer.generation;
        char bundle_path[128];
        snprintf(bundle_path, sizeof(bundle_path), "%s/%s", M3_BUNDLES_DIR, pointer.bundle_name);
        struct stat bundle;
        const bool valid = stat(bundle_path, &bundle) == 0 &&
                           bundle.st_size >= (off_t) sizeof(m3_bundle_header_t) &&
                           bundle.st_size <= M3_MAX_BUNDLE_BYTES &&
                           m3_validate_bundle_file(bundle_path, (uint32_t) bundle.st_size,
                                                   pointer.bundle_crc32);
        if (valid && valid_count == 0) {
            s_storage.active_generation = pointer.generation;
            s_storage.active_bundle_valid = true;
            snprintf(s_active_bundle_path, sizeof(s_active_bundle_path), "%s", bundle_path);
            ++valid_count;
        } else if (valid) {
            s_storage.previous_generation = pointer.generation;
            break;
        } else {
            ESP_LOGW(TAG, "M3_SD result=invalid_generation generation=%u", pointer.generation);
            unlink(pointer_path);
        }
    }
    m3_cleanup_generations();
}

static bool m3_storage_init(void)
{
    if (bsp_sdcard_mount() != ESP_OK) {
        ESP_LOGE(TAG, "M3_SD result=mount_failed");
        return false;
    }
    s_storage.mounted = m3_mkdir(M3_ROOT) && m3_mkdir(M3_BUNDLES_DIR);
    if (!s_storage.mounted) {
        ESP_LOGE(TAG, "M3_SD result=directory_init_failed");
        return false;
    }
    struct stat media;
    if (stat(M3_MEDIA_FILE, &media) != 0 && stat(M3_MEDIA_BACKUP_FILE, &media) == 0 &&
        rename(M3_MEDIA_BACKUP_FILE, M3_MEDIA_FILE) == 0) {
        ESP_LOGW(TAG, "M3_MEDIA action=recover_previous");
    }
    m3_find_active_pointer();
    if (!m3_upload_state_load()) {
        unlink(M3_STATE_FILE);
        ESP_LOGI(TAG, "M3_SD result=ready active_generation=%u active_valid=%u", s_storage.active_generation, s_storage.active_bundle_valid);
    } else {
        ESP_LOGW(TAG, "M3_SD result=resume_available bytes=%u/%u", s_storage.received_bytes, s_storage.total_bytes);
    }
    return true;
}

static void m3_send_response(uint8_t opcode, uint32_t sequence, m3_status_t status, uint32_t value)
{
    struct __attribute__((packed)) {
        m3_frame_header_t header;
        uint32_t status;
        uint32_t value;
    } response = {
        .header = {
            .magic = M3_PROTOCOL_MAGIC,
            .version = M3_PROTOCOL_VERSION,
            .opcode = opcode | 0x80,
            .sequence = sequence,
            .payload_size = 8,
        },
        .status = status,
        .value = value,
    };
    response.header.payload_crc32 = m3_crc32(&response.status, sizeof(response.status) + sizeof(response.value));
    if (!s_usb_mounted) return;
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(M3_RESPONSE_WAIT_MS);
    while (s_usb_mounted && tud_vendor_n_write_available(0) < sizeof(response) &&
           xTaskGetTickCount() < deadline) {
        vTaskDelay(1);
    }
    if (!s_usb_mounted || tud_vendor_n_write_available(0) < sizeof(response)) {
        ESP_LOGE(TAG, "M3_SYNC result=response_timeout opcode=%u sequence=%u", opcode, sequence);
        return;
    }
    const uint32_t written = tud_vendor_n_write(0, &response, sizeof(response));
    tud_vendor_n_write_flush(0);
    if (written != sizeof(response)) {
        ESP_LOGE(TAG, "M3_SYNC result=response_short opcode=%u sequence=%u bytes=%u",
                 opcode, sequence, written);
    }
}

static void m3_send_payload(uint8_t opcode, uint32_t sequence, const void *payload, uint32_t payload_size)
{
    m3_frame_header_t header = {
        .magic = M3_PROTOCOL_MAGIC, .version = M3_PROTOCOL_VERSION,
        .opcode = opcode | 0x80, .sequence = sequence, .payload_size = payload_size,
        .payload_crc32 = m3_crc32(payload, payload_size),
    };
    const uint32_t total = sizeof(header) + payload_size;
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(M3_RESPONSE_WAIT_MS);
    while (s_usb_mounted && tud_vendor_n_write_available(0) < total && xTaskGetTickCount() < deadline) vTaskDelay(1);
    if (!s_usb_mounted || tud_vendor_n_write_available(0) < total ||
        tud_vendor_n_write(0, &header, sizeof(header)) != sizeof(header) ||
        tud_vendor_n_write(0, payload, payload_size) != payload_size) {
        ESP_LOGE(TAG, "M3_SYNC result=download_response_failed sequence=%u", sequence);
        return;
    }
    tud_vendor_n_write_flush(0);
}

static void m3_handle_download_chunk(const m3_frame_header_t *frame, const uint8_t *payload)
{
    if (!s_storage.active_bundle_valid || s_download_file == NULL || frame->payload_size != sizeof(uint32_t)) {
        m3_send_response(M3_OP_DOWNLOAD_CHUNK, frame->sequence, M3_STATUS_BAD_STATE, 0); return;
    }
    uint32_t offset;
    memcpy(&offset, payload, sizeof(offset));
    if (offset != s_download_offset && fseek(s_download_file, (long) offset, SEEK_SET) != 0) {
        m3_send_response(M3_OP_DOWNLOAD_CHUNK, frame->sequence, M3_STATUS_IO, offset); return;
    }
    uint8_t chunk[M3_MAX_FRAME_PAYLOAD];
    const size_t count = fread(chunk, 1, sizeof(chunk), s_download_file);
    s_download_offset = offset + count;
    m3_send_payload(M3_OP_DOWNLOAD_CHUNK, frame->sequence, chunk, count);
}

static void m3_handle_download_begin(const m3_frame_header_t *frame)
{
    if (s_download_file != NULL) fclose(s_download_file);
    s_download_file = NULL;
    s_download_offset = 0;
    struct stat info;
    if (!s_storage.active_bundle_valid || stat(s_active_bundle_path, &info) != 0 ||
        (s_download_file = fopen(s_active_bundle_path, "rb")) == NULL) {
        m3_send_response(M3_OP_DOWNLOAD_BEGIN, frame->sequence, M3_STATUS_BAD_STATE, 0);
        return;
    }
    m3_send_response(M3_OP_DOWNLOAD_BEGIN, frame->sequence, M3_STATUS_OK, (uint32_t) info.st_size);
}

static void m3_handle_begin(const m3_frame_header_t *frame, const uint8_t *payload)
{
    if (!s_storage.mounted || frame->payload_size != sizeof(m3_begin_t)) {
        m3_send_response(M3_OP_BEGIN, frame->sequence, M3_STATUS_BAD_FRAME, 0);
        return;
    }
    m3_begin_t begin;
    memcpy(&begin, payload, sizeof(begin));
    if (begin.total_bytes < sizeof(m3_bundle_header_t) || begin.total_bytes > M3_MAX_BUNDLE_BYTES) {
        m3_send_response(M3_OP_BEGIN, frame->sequence, M3_STATUS_BAD_BUNDLE, 0);
        return;
    }
    if (s_storage.upload_open && s_storage.total_bytes == begin.total_bytes &&
        s_storage.bundle_crc32 == begin.bundle_crc32) {
        if (!m3_bundle_upload_open(true)) {
            m3_send_response(M3_OP_BEGIN, frame->sequence, M3_STATUS_IO, s_storage.durable_bytes);
            return;
        }
        m3_send_response(M3_OP_BEGIN, frame->sequence, M3_STATUS_OK, s_storage.received_bytes);
        return;
    }
    m3_bundle_upload_close();
    unlink(M3_STAGE_FILE);
    unlink(M3_STATE_FILE);
    s_storage.upload_open = true;
    s_storage.total_bytes = begin.total_bytes;
    s_storage.bundle_crc32 = begin.bundle_crc32;
    s_storage.received_bytes = 0;
    s_storage.durable_bytes = 0;
    if (!m3_bundle_upload_open(false) || !m3_upload_state_write()) {
        m3_bundle_upload_close();
        s_storage.upload_open = false;
        m3_send_response(M3_OP_BEGIN, frame->sequence, M3_STATUS_IO, 0);
        return;
    }
    ESP_LOGI(TAG, "M3_SYNC action=begin bytes=%u crc=%08" PRIx32, begin.total_bytes, begin.bundle_crc32);
    m3_send_response(M3_OP_BEGIN, frame->sequence, M3_STATUS_OK, 0);
}

static void m3_handle_chunk(const m3_frame_header_t *frame, const uint8_t *payload, bool respond)
{
    if (!s_storage.upload_open || frame->payload_size < sizeof(m3_chunk_prefix_t)) {
        if (respond) m3_send_response(M3_OP_CHUNK, frame->sequence, M3_STATUS_BAD_STATE, s_storage.durable_bytes);
        return;
    }
    m3_chunk_prefix_t prefix;
    memcpy(&prefix, payload, sizeof(prefix));
    const uint8_t *chunk = payload + sizeof(prefix);
    const size_t chunk_size = frame->payload_size - sizeof(prefix);
    if (prefix.offset != s_storage.received_bytes || chunk_size == 0 ||
        chunk_size > M3_MAX_FRAME_PAYLOAD - sizeof(prefix) ||
        chunk_size > s_storage.total_bytes - s_storage.received_bytes ||
        prefix.chunk_crc32 != m3_crc32(chunk, chunk_size)) {
        if (respond) m3_send_response(M3_OP_CHUNK, frame->sequence, M3_STATUS_BAD_FRAME, s_storage.durable_bytes);
        return;
    }
    bool ok = m3_bundle_upload_open(true);
    if (ok && s_storage.upload_buffer_used + chunk_size > M3_BUNDLE_WRITE_BUFFER_BYTES) {
        ok = m3_bundle_upload_flush_buffer();
    }
    if (ok) {
        memcpy(s_storage.upload_buffer + s_storage.upload_buffer_used, chunk, chunk_size);
        s_storage.upload_buffer_used += chunk_size;
    }
    if (!ok) {
        if (respond) m3_send_response(M3_OP_CHUNK, frame->sequence, M3_STATUS_IO, s_storage.durable_bytes);
        return;
    }
    s_storage.received_bytes += chunk_size;
    if (respond && !m3_bundle_upload_checkpoint()) {
        m3_send_response(M3_OP_CHUNK, frame->sequence, M3_STATUS_IO, s_storage.durable_bytes);
        return;
    }
    if (respond) m3_send_response(M3_OP_CHUNK, frame->sequence, M3_STATUS_OK, s_storage.durable_bytes);
}

static void m3_handle_commit(const m3_frame_header_t *frame)
{
    const bool checkpointed = s_storage.upload_open &&
                              s_storage.received_bytes == s_storage.total_bytes &&
                              m3_bundle_upload_checkpoint();
    m3_bundle_upload_close();
    if (!checkpointed ||
        !m3_validate_bundle_file(M3_STAGE_FILE, s_storage.total_bytes, s_storage.bundle_crc32)) {
        m3_send_response(M3_OP_COMMIT, frame->sequence, M3_STATUS_BAD_BUNDLE, s_storage.received_bytes);
        return;
    }
    const uint32_t generation = s_storage.active_generation + 1;
    char bundle_name[32];
    char bundle_path[128];
    char pointer_path[128];
    snprintf(bundle_name, sizeof(bundle_name), "bundle-%08" PRIu32 ".sdb", generation);
    snprintf(bundle_path, sizeof(bundle_path), "%s/%s", M3_BUNDLES_DIR, bundle_name);
    snprintf(pointer_path, sizeof(pointer_path), "%s/active-%08" PRIu32 ".ptr", M3_ROOT, generation);
    if (rename(M3_STAGE_FILE, bundle_path) != 0) {
        m3_send_response(M3_OP_COMMIT, frame->sequence, M3_STATUS_IO, s_storage.received_bytes);
        return;
    }
    m3_pointer_t pointer = {.magic = M3_POINTER_MAGIC, .generation = generation, .bundle_crc32 = s_storage.bundle_crc32};
    const size_t bundle_name_length = strnlen(bundle_name, sizeof(pointer.bundle_name) - 1);
    memcpy(pointer.bundle_name, bundle_name, bundle_name_length);
    pointer.bundle_name[bundle_name_length] = '\0';
    pointer.record_crc32 = m3_crc32(&pointer, offsetof(m3_pointer_t, record_crc32));
    if (!m3_write_exact(pointer_path, &pointer, sizeof(pointer))) {
        // Bundle remains unreferenced; old pointer is still valid and boot-safe.
        m3_send_response(M3_OP_COMMIT, frame->sequence, M3_STATUS_IO, s_storage.received_bytes);
        return;
    }
    unlink(M3_STATE_FILE);
    s_storage.previous_generation = s_storage.active_generation;
    s_storage.active_generation = generation;
    s_storage.active_bundle_valid = true;
    snprintf(s_active_bundle_path, sizeof(s_active_bundle_path), "%s", bundle_path);
    s_storage.upload_open = false;
    s_storage.received_bytes = 0;
    s_storage.durable_bytes = 0;
    m3_cleanup_generations();
    ESP_LOGI(TAG, "M3_SYNC action=commit generation=%u bundle=%s", generation, bundle_name);
    m3_send_response(M3_OP_COMMIT, frame->sequence, M3_STATUS_OK, generation);
#ifdef M5_MEDIA_ENABLED
    BaseType_t restart_ok = xTaskCreate(m3_restart_after_commit, "bundle_restart", 2048,
                                        NULL, 4, NULL);
    if (restart_ok != pdPASS) ESP_LOGE(TAG, "M3_SYNC result=restart_task_failed");
#endif
}

static bool m3_validate_mjpeg_file(const char *path, uint32_t expected_bytes, uint32_t expected_crc)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) return false;
    uint8_t block[512];
    uint32_t crc = UINT32_MAX;
    uint32_t bytes = 0;
    uint8_t first[2] = {0};
    uint8_t last[2] = {0};
    size_t read;
    while ((read = fread(block, 1, sizeof(block), file)) != 0) {
        if (bytes == 0 && read >= 2) memcpy(first, block, 2);
        for (size_t index = 0; index < read; ++index) {
            last[0] = last[1]; last[1] = block[index];
        }
        crc = esp_crc32_le(crc, block, read);
        bytes += read;
    }
    fclose(file);
    return bytes == expected_bytes && crc == expected_crc &&
           first[0] == 0xFF && first[1] == 0xD8 && last[0] == 0xFF && last[1] == 0xD9;
}

static void m3_handle_media_begin(const m3_frame_header_t *frame, const uint8_t *payload)
{
    if (!s_storage.mounted || frame->payload_size != sizeof(m3_begin_t)) {
        m3_send_response(M3_OP_MEDIA_BEGIN, frame->sequence, M3_STATUS_BAD_FRAME, 0); return;
    }
    m3_begin_t begin; memcpy(&begin, payload, sizeof(begin));
    if (begin.total_bytes < 4 || begin.total_bytes > M3_MAX_MEDIA_BYTES) {
        m3_send_response(M3_OP_MEDIA_BEGIN, frame->sequence, M3_STATUS_BAD_BUNDLE, 0); return;
    }
    if (s_media_upload.open && s_media_upload.total_bytes == begin.total_bytes && s_media_upload.crc32 == begin.bundle_crc32) {
        m3_send_response(M3_OP_MEDIA_BEGIN, frame->sequence, M3_STATUS_OK, s_media_upload.received_bytes); return;
    }
    if (s_media_upload.file != NULL) fclose(s_media_upload.file);
    free(s_media_upload.write_buffer);
    unlink(M3_MEDIA_STAGE_FILE);
    FILE *file = fopen(M3_MEDIA_STAGE_FILE, "wb");
    if (file == NULL) {
        s_media_upload = (m3_media_upload_t) {0};
        m3_send_response(M3_OP_MEDIA_BEGIN, frame->sequence, M3_STATUS_IO, 0); return;
    }
    uint8_t *write_buffer = malloc(M3_MEDIA_WRITE_BUFFER_BYTES);
    if (write_buffer == NULL) {
        free(write_buffer);
        fclose(file); unlink(M3_MEDIA_STAGE_FILE);
        s_media_upload = (m3_media_upload_t) {0};
        m3_send_response(M3_OP_MEDIA_BEGIN, frame->sequence, M3_STATUS_IO, 0); return;
    }
    s_media_upload = (m3_media_upload_t) {
        .open = true, .file = file, .write_buffer = write_buffer,
        .total_bytes = begin.total_bytes, .crc32 = begin.bundle_crc32,
    };
    ESP_LOGI(TAG, "M3_MEDIA action=begin bytes=%u crc=%08" PRIx32, begin.total_bytes, begin.bundle_crc32);
    m3_send_response(M3_OP_MEDIA_BEGIN, frame->sequence, M3_STATUS_OK, 0);
}

static void m3_handle_media_chunk(const m3_frame_header_t *frame, const uint8_t *payload, bool respond)
{
    if (!s_media_upload.open || frame->payload_size < sizeof(m3_chunk_prefix_t)) {
        if (respond) m3_send_response(M3_OP_MEDIA_CHUNK, frame->sequence, M3_STATUS_BAD_STATE, s_media_upload.received_bytes);
        return;
    }
    m3_chunk_prefix_t prefix; memcpy(&prefix, payload, sizeof(prefix));
    const uint8_t *chunk = payload + sizeof(prefix);
    const size_t chunk_size = frame->payload_size - sizeof(prefix);
    if (prefix.offset != s_media_upload.received_bytes || chunk_size == 0 ||
        chunk_size > M3_MAX_FRAME_PAYLOAD - sizeof(prefix) ||
        chunk_size > s_media_upload.total_bytes - s_media_upload.received_bytes ||
        prefix.chunk_crc32 != m3_crc32(chunk, chunk_size)) {
        if (respond) m3_send_response(M3_OP_MEDIA_CHUNK, frame->sequence, M3_STATUS_BAD_FRAME, s_media_upload.received_bytes);
        return;
    }
    bool ok = s_media_upload.file != NULL && s_media_upload.write_buffer != NULL;
    if (ok && s_media_upload.write_buffer_used + chunk_size > M3_MEDIA_WRITE_BUFFER_BYTES) {
        ok = fwrite(s_media_upload.write_buffer, 1, s_media_upload.write_buffer_used,
                    s_media_upload.file) == s_media_upload.write_buffer_used;
        s_media_upload.write_buffer_used = 0;
    }
    if (ok) {
        memcpy(s_media_upload.write_buffer + s_media_upload.write_buffer_used, chunk, chunk_size);
        s_media_upload.write_buffer_used += chunk_size;
    }
    if (!ok) {
        if (respond) m3_send_response(M3_OP_MEDIA_CHUNK, frame->sequence, M3_STATUS_IO, s_media_upload.received_bytes);
        return;
    }
    s_media_upload.received_bytes += chunk_size;
    if (respond) m3_send_response(M3_OP_MEDIA_CHUNK, frame->sequence, M3_STATUS_OK, s_media_upload.received_bytes);
}

static void m3_handle_media_commit(const m3_frame_header_t *frame)
{
    if (s_media_upload.file != NULL) {
        bool flushed = s_media_upload.write_buffer != NULL &&
                       fwrite(s_media_upload.write_buffer, 1, s_media_upload.write_buffer_used,
                              s_media_upload.file) == s_media_upload.write_buffer_used;
        flushed = flushed && fflush(s_media_upload.file) == 0 && fsync(fileno(s_media_upload.file)) == 0;
        fclose(s_media_upload.file);
        s_media_upload.file = NULL;
        if (!flushed) {
            m3_send_response(M3_OP_MEDIA_COMMIT, frame->sequence, M3_STATUS_IO, s_media_upload.received_bytes); return;
        }
    }
    free(s_media_upload.write_buffer);
    s_media_upload.write_buffer = NULL;
    if (!s_media_upload.open || s_media_upload.received_bytes != s_media_upload.total_bytes ||
        !m3_validate_mjpeg_file(M3_MEDIA_STAGE_FILE, s_media_upload.total_bytes, s_media_upload.crc32)) {
        m3_send_response(M3_OP_MEDIA_COMMIT, frame->sequence, M3_STATUS_BAD_BUNDLE, s_media_upload.received_bytes); return;
    }
    /* FATFS cannot replace an existing name with rename. Preserve the last
     * known-good file under a recovery name before activating the new one. */
    unlink(M3_MEDIA_BACKUP_FILE);
    struct stat previous_media;
    const bool had_previous = stat(M3_MEDIA_FILE, &previous_media) == 0;
    if (had_previous && rename(M3_MEDIA_FILE, M3_MEDIA_BACKUP_FILE) != 0) {
        m3_send_response(M3_OP_MEDIA_COMMIT, frame->sequence, M3_STATUS_IO, s_media_upload.received_bytes); return;
    }
    if (rename(M3_MEDIA_STAGE_FILE, M3_MEDIA_FILE) != 0) {
        if (had_previous) rename(M3_MEDIA_BACKUP_FILE, M3_MEDIA_FILE);
        m3_send_response(M3_OP_MEDIA_COMMIT, frame->sequence, M3_STATUS_IO, s_media_upload.received_bytes); return;
    }
    const uint32_t uploaded = s_media_upload.total_bytes;
    s_media_upload = (m3_media_upload_t) {0};
    ESP_LOGI(TAG, "M3_MEDIA action=commit bytes=%u path=%s", uploaded, M3_MEDIA_FILE);
    m3_send_response(M3_OP_MEDIA_COMMIT, frame->sequence, M3_STATUS_OK, uploaded);
#ifdef M5_MEDIA_ENABLED
    BaseType_t restart_ok = xTaskCreate(m3_restart_after_commit, "media_restart", 2048,
                                        NULL, 4, NULL);
    if (restart_ok != pdPASS) ESP_LOGE(TAG, "M3_MEDIA result=restart_task_failed");
#endif
}

static void m3_dispatch_frame(const m3_frame_header_t *frame, const uint8_t *payload)
{
    if (frame->opcode == M3_OP_HELLO) {
        // Protocol v1, resume, checksums, atomic bundles, no MSC, media upload,
        // and batched media/bundle chunks (silent intermediate frames).
        m3_send_response(M3_OP_HELLO, frame->sequence, M3_STATUS_OK, 0x000003FF);
    } else if (frame->opcode == M3_OP_BEGIN) {
        m3_handle_begin(frame, payload);
    } else if (frame->opcode == M3_OP_CHUNK) {
        m3_handle_chunk(frame, payload,
                        (frame->reserved & M3_FRAME_FLAG_NO_RESPONSE) == 0);
    } else if (frame->opcode == M3_OP_COMMIT) {
        m3_handle_commit(frame);
    } else if (frame->opcode == M3_OP_MEDIA_BEGIN) {
        m3_handle_media_begin(frame, payload);
    } else if (frame->opcode == M3_OP_MEDIA_CHUNK) {
        m3_handle_media_chunk(frame, payload,
                              (frame->reserved & M3_FRAME_FLAG_NO_RESPONSE) == 0);
    } else if (frame->opcode == M3_OP_MEDIA_COMMIT) {
        m3_handle_media_commit(frame);
    } else if (frame->opcode == M3_OP_DOWNLOAD_BEGIN) {
        m3_handle_download_begin(frame);
    } else if (frame->opcode == M3_OP_DOWNLOAD_CHUNK) {
        m3_handle_download_chunk(frame, payload);
    } else if (frame->opcode == M3_OP_MEDIA_ABORT) {
        if (s_media_upload.file != NULL) fclose(s_media_upload.file);
        free(s_media_upload.write_buffer);
        unlink(M3_MEDIA_STAGE_FILE); s_media_upload = (m3_media_upload_t) {0};
        m3_send_response(M3_OP_MEDIA_ABORT, frame->sequence, M3_STATUS_OK, 0);
#ifdef M5_MEDIA_ENABLED
    } else if (frame->opcode == M3_OP_TEST_SCREENSAVER) {
        const uint32_t media_error = m5_media_trigger_screensaver();
        m3_send_response(M3_OP_TEST_SCREENSAVER, frame->sequence,
                         media_error == 0 ? M3_STATUS_OK : M3_STATUS_MEDIA_UNAVAILABLE,
                         media_error);
#endif
    } else if (frame->opcode == M3_OP_ABORT) {
        m3_bundle_upload_close();
        unlink(M3_STAGE_FILE); unlink(M3_STATE_FILE);
        s_storage.upload_open = false; s_storage.received_bytes = 0; s_storage.durable_bytes = 0;
        m3_send_response(M3_OP_ABORT, frame->sequence, M3_STATUS_OK, 0);
    } else if (frame->opcode == M3_OP_STATUS || frame->opcode == M3_OP_DIAG) {
        m3_send_response(frame->opcode, frame->sequence, M3_STATUS_OK,
                         s_storage.upload_open ? s_storage.received_bytes : s_storage.active_generation);
    } else {
        m3_send_response(frame->opcode, frame->sequence, M3_STATUS_BAD_FRAME, 0);
    }
}

static void m3_feed_bytes(const uint8_t *bytes, size_t length)
{
    while (length > 0) {
        const size_t room = sizeof(s_frame_buffer) - s_frame_length;
        const size_t copy = length < room ? length : room;
        memcpy(s_frame_buffer + s_frame_length, bytes, copy);
        s_frame_length += copy;
        bytes += copy;
        length -= copy;
        if (s_frame_length < sizeof(m3_frame_header_t)) continue;
        m3_frame_header_t frame;
        memcpy(&frame, s_frame_buffer, sizeof(frame));
        const size_t full_length = sizeof(frame) + frame.payload_size;
        if (frame.magic != M3_PROTOCOL_MAGIC || frame.version != M3_PROTOCOL_VERSION ||
            frame.payload_size > M3_MAX_FRAME_PAYLOAD || full_length > sizeof(s_frame_buffer)) {
            s_frame_length = 0;
            m3_send_response(0, frame.sequence, M3_STATUS_BAD_FRAME, 0);
            continue;
        }
        if (s_frame_length < full_length) continue;
        const uint8_t *payload = s_frame_buffer + sizeof(frame);
        if (frame.payload_crc32 != m3_crc32(payload, frame.payload_size)) {
            m3_send_response(frame.opcode, frame.sequence, M3_STATUS_BAD_FRAME, s_storage.received_bytes);
        } else {
            m3_dispatch_frame(&frame, payload);
        }
        const size_t remaining = s_frame_length - full_length;
        memmove(s_frame_buffer, s_frame_buffer + full_length, remaining);
        s_frame_length = remaining;
    }
}

static void m3_sync_task(void *argument)
{
    (void) argument;
    m3_rx_packet_t packet;
    while (true) {
        if (xQueueReceive(s_rx_queue, &packet, portMAX_DELAY) == pdTRUE) {
            m3_feed_bytes(packet.bytes, packet.length);
        }
    }
}

void tud_vendor_rx_cb(uint8_t itf, uint8_t const *buffer, uint16_t bufsize)
{
    if (itf != 0 || s_rx_queue == NULL || buffer == NULL || bufsize == 0) return;
    if (bufsize > M3_RX_PACKET_BYTES) {
        ESP_LOGE(TAG, "M3_SYNC result=rx_frame_too_large bytes=%u", bufsize);
        return;
    }
    m3_rx_packet_t packet = {.length = bufsize};
    memcpy(packet.bytes, buffer, packet.length);
    if (xQueueSend(s_rx_queue, &packet, 0) != pdTRUE) {
        ESP_LOGW(TAG, "M3_SYNC result=rx_queue_full");
    }
    /* This TinyUSB fork mirrors callback bytes into its vendor RX FIFO before
     * invoking us. We consume the callback buffer directly, so drain that
     * duplicate FIFO or the OUT endpoint stops rearming after 2 KiB. */
    tud_vendor_n_read_flush(0);
}

static void m3_usb_event_cb(tinyusb_event_t *event, void *argument)
{
    (void) argument;
    if (event->id == TINYUSB_EVENT_ATTACHED) {
        s_usb_mounted = true;
#ifdef M5_MEDIA_ENABLED
        m5_hid_release_all("usb_attached");
#endif
        ESP_LOGI(TAG, "M3_USB state=mounted interfaces=keyboard,vendor_sync");
    } else if (event->id == TINYUSB_EVENT_DETACHED) {
        s_usb_mounted = false;
        if (s_download_file != NULL) fclose(s_download_file);
        s_download_file = NULL;
        s_download_offset = 0;
#ifdef M5_MEDIA_ENABLED
        m5_hid_release_all("usb_detached");
#endif
        ESP_LOGW(TAG, "M3_USB state=unmounted transfer_resume=%u", s_storage.upload_open);
    }
}

static void m3_usb_init(void)
{
    tinyusb_config_t config = TINYUSB_DEFAULT_CONFIG();
    config.descriptor.device = &s_device_descriptor;
    config.descriptor.qualifier = &s_device_qualifier;
    config.descriptor.full_speed_config = s_full_speed_configuration_descriptor;
    config.descriptor.string = s_string_descriptors;
    config.descriptor.string_count = sizeof(s_string_descriptors) / sizeof(s_string_descriptors[0]);
    config.event_cb = m3_usb_event_cb;
#if (TUD_OPT_HIGH_SPEED)
    config.descriptor.high_speed_config = s_high_speed_configuration_descriptor;
#endif
    ESP_ERROR_CHECK(tinyusb_driver_install(&config));
}

static void m3_recovery_ui(void)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_t *card = lv_obj_create(screen);
    lv_obj_set_size(card, 260, 260);
    lv_obj_center(card);
    lv_obj_set_style_radius(card, 28, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(s_storage.active_bundle_valid ? 0x14532D : 0x7C2D12), 0);
    lv_obj_t *mark = lv_label_create(card);
    lv_label_set_text(mark, s_storage.active_bundle_valid ? LV_SYMBOL_OK : LV_SYMBOL_WARNING);
    lv_obj_set_style_text_color(mark, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(mark);
}

void app_main(void)
{
    ESP_LOGI(TAG, "M3_START idf=%s target=esp32p4", esp_get_idf_version());
    const bool storage_ok = m3_storage_init();
    s_rx_queue = xQueueCreate(M3_RX_QUEUE_DEPTH, sizeof(m3_rx_packet_t));
    ESP_ERROR_CHECK(s_rx_queue == NULL ? ESP_ERR_NO_MEM : ESP_OK);
    ESP_ERROR_CHECK(xTaskCreate(m3_sync_task, "m3_sync", 6144, NULL, 6, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    m3_usb_init();

    bsp_display_cfg_t display_cfg = {
        .lv_adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG(),
        .rotation = ESP_LV_ADAPTER_ROTATE_90,
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_PARTIAL,
        .touch_flags = {.swap_xy = true, .mirror_x = true, .mirror_y = false},
    };
    s_display = bsp_display_start_with_config(&display_cfg);
    if (s_display != NULL) {
        bsp_display_backlight_on();
        ESP_ERROR_CHECK(esp_lv_adapter_lock(UINT32_MAX));
        m3_recovery_ui();
        esp_lv_adapter_unlock();
#ifdef M5_MEDIA_ENABLED
        m5_media_start(s_display);
#endif
    }
#ifdef M5_MEDIA_ENABLED
    ESP_LOGI(TAG, "M3_COMPLETE sd_ready=%u active_bundle=%u hid_output=macro_runtime msc=disabled",
#else
    ESP_LOGI(TAG, "M3_COMPLETE sd_ready=%u active_bundle=%u hid_output=disabled msc=disabled",
#endif
             storage_ok, s_storage.active_bundle_valid);
}
