/*
 * M2 — autonomous runtime UI and safe macro engine.
 *
 * M3 replaces the compiled default bundle with a microSD bundle.  Keeping the
 * same versioned, bounded model here makes M2 exercise the real recovery and
 * scheduler paths without giving the host ownership of the device.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "class/hid/hid_device.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"

static const char *TAG = "m2";

#define M2_COLUMNS 8
#define M2_ROWS 4
#define M2_BUTTONS_PER_PAGE (M2_COLUMNS * M2_ROWS)
#define M2_MAX_PAGES 2
#define M2_MAX_PROFILES 2
#define M2_MAX_MACROS 12
#define M2_MAX_STEPS 8
#define M2_MAX_SLOTS 8
#define M2_HID_REPORT_ID 1
#define M2_BUNDLE_MAGIC 0x324B4453UL /* SDK2 */
#define M2_BUNDLE_VERSION 1
#define M2_GAP_PX 8
#define M2_MARGIN_X_PX 16

typedef enum {
    M2_STEP_KEY_DOWN,
    M2_STEP_KEY_UP,
    M2_STEP_MOD_DOWN,
    M2_STEP_MOD_UP,
    M2_STEP_DELAY,
} m2_step_type_t;

typedef struct {
    uint8_t type;
    uint8_t value;
    uint16_t duration_ms;
} m2_step_t;

typedef struct {
    uint8_t step_count;
    m2_step_t steps[M2_MAX_STEPS];
} m2_macro_t;

typedef enum {
    M2_ACTION_MACRO,
    M2_ACTION_PAGE_NEXT,
    M2_ACTION_PAGE_PREVIOUS,
    M2_ACTION_PROFILE_NEXT,
} m2_action_type_t;

typedef struct {
    uint8_t icon_id;
    uint8_t action_type;
    uint8_t action_arg;
} m2_button_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint8_t profile_count;
    uint8_t page_count;
    uint8_t buttons_per_page;
    uint8_t macro_count;
} m2_bundle_header_t;

typedef struct {
    m2_bundle_header_t header;
    m2_macro_t macros[M2_MAX_MACROS];
    m2_button_t buttons[M2_MAX_PAGES][M2_BUTTONS_PER_PAGE];
} m2_bundle_t;

typedef struct {
    bool active;
    uint8_t owner_button;
    uint8_t macro_id;
    uint8_t next_step;
    int64_t due_us;
    bool held_keys[256];
    uint8_t held_modifiers;
} m2_slot_t;

typedef struct {
    uint32_t color;
    const char *symbol;
} m2_icon_t;

static m2_bundle_t s_bundle;
static m2_slot_t s_slots[M2_MAX_SLOTS];
static uint8_t s_key_refcount[256];
static uint8_t s_modifier_refcount[8];
static SemaphoreHandle_t s_engine_lock;
static bool s_usb_mounted;
static uint8_t s_current_page;
static uint8_t s_current_profile;
static lv_display_t *s_display;

static const m2_icon_t s_icon_cache[] = {
    {0x0E7490, LV_SYMBOL_PLAY}, {0x1D4ED8, LV_SYMBOL_OK},
    {0x4338CA, LV_SYMBOL_SETTINGS}, {0x7E22CE, LV_SYMBOL_LOOP},
    {0xBE123C, LV_SYMBOL_CHARGE}, {0xC2410C, LV_SYMBOL_POWER},
    {0xA16207, LV_SYMBOL_EYE_OPEN}, {0x4D7C0F, LV_SYMBOL_HOME},
    {0x0F766E, LV_SYMBOL_SAVE}, {0x0369A1, LV_SYMBOL_DOWNLOAD},
    {0x6D28D9, LV_SYMBOL_UPLOAD}, {0x9D174D, LV_SYMBOL_BELL},
    {0x111827, LV_SYMBOL_LEFT}, {0x111827, LV_SYMBOL_RIGHT},
    {0x111827, LV_SYMBOL_REFRESH},
};

static const uint8_t s_hid_report_descriptor[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(M2_HID_REPORT_ID)),
};

static const char *s_hid_string_descriptors[] = {
    (const char[]) {0x09, 0x04}, "Screendeck", "Screendeck M2 Macro Runtime",
    "M2-MACRO-RUNTIME", "Keyboard",
};

#define M2_TUSB_DESCRIPTOR_LENGTH (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN)
static const uint8_t s_hid_configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, M2_TUSB_DESCRIPTOR_LENGTH,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_DESCRIPTOR(0, 4, HID_ITF_PROTOCOL_KEYBOARD,
                       sizeof(s_hid_report_descriptor), 0x81, 16, 10),
};

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

static void m2_build_default_bundle(m2_bundle_t *bundle)
{
    memset(bundle, 0, sizeof(*bundle));
    bundle->header = (m2_bundle_header_t) {
        .magic = M2_BUNDLE_MAGIC,
        .version = M2_BUNDLE_VERSION,
        .profile_count = M2_MAX_PROFILES,
        .page_count = M2_MAX_PAGES,
        .buttons_per_page = M2_BUTTONS_PER_PAGE,
        .macro_count = M2_MAX_MACROS,
    };

    for (uint8_t macro = 0; macro < M2_MAX_MACROS; ++macro) {
        m2_macro_t *definition = &bundle->macros[macro];
        const uint8_t key = HID_KEY_F13 + macro;
        definition->steps[0] = (m2_step_t) {.type = M2_STEP_KEY_DOWN, .value = key};
        definition->steps[1] = (m2_step_t) {.type = M2_STEP_DELAY, .duration_ms = 25};
        definition->steps[2] = (m2_step_t) {.type = M2_STEP_KEY_UP, .value = key};
        definition->step_count = 3;
    }

    // Exercise explicit modifiers and central key accounting with a common
    // Ctrl+Shift+F24 chord. The corresponding releases are still slot-owned.
    bundle->macros[11] = (m2_macro_t) {
        .step_count = 7,
        .steps = {
            {.type = M2_STEP_MOD_DOWN, .value = KEYBOARD_MODIFIER_LEFTCTRL},
            {.type = M2_STEP_MOD_DOWN, .value = KEYBOARD_MODIFIER_LEFTSHIFT},
            {.type = M2_STEP_KEY_DOWN, .value = HID_KEY_F24},
            {.type = M2_STEP_DELAY, .duration_ms = 25},
            {.type = M2_STEP_KEY_UP, .value = HID_KEY_F24},
            {.type = M2_STEP_MOD_UP, .value = KEYBOARD_MODIFIER_LEFTSHIFT},
            {.type = M2_STEP_MOD_UP, .value = KEYBOARD_MODIFIER_LEFTCTRL},
        },
    };

    for (uint8_t page = 0; page < M2_MAX_PAGES; ++page) {
        for (uint8_t index = 0; index < M2_BUTTONS_PER_PAGE; ++index) {
            bundle->buttons[page][index] = (m2_button_t) {
                .icon_id = (uint8_t) ((index + (page * 3)) % 12),
                .action_type = M2_ACTION_MACRO,
                .action_arg = (uint8_t) ((index + page) % M2_MAX_MACROS),
            };
        }
        bundle->buttons[page][30] = (m2_button_t) {
            .icon_id = 12, .action_type = M2_ACTION_PAGE_PREVIOUS,
        };
        bundle->buttons[page][31] = (m2_button_t) {
            .icon_id = page == 0 ? 13 : 14,
            .action_type = page == 0 ? M2_ACTION_PAGE_NEXT : M2_ACTION_PROFILE_NEXT,
        };
    }
}

static bool m2_validate_bundle(const m2_bundle_t *bundle)
{
    const m2_bundle_header_t *header = &bundle->header;
    if (header->magic != M2_BUNDLE_MAGIC || header->version != M2_BUNDLE_VERSION ||
        header->profile_count == 0 || header->profile_count > M2_MAX_PROFILES ||
        header->page_count == 0 || header->page_count > M2_MAX_PAGES ||
        header->buttons_per_page != M2_BUTTONS_PER_PAGE ||
        header->macro_count == 0 || header->macro_count > M2_MAX_MACROS) {
        return false;
    }
    for (uint8_t macro = 0; macro < header->macro_count; ++macro) {
        const m2_macro_t *definition = &bundle->macros[macro];
        if (definition->step_count == 0 || definition->step_count > M2_MAX_STEPS) {
            return false;
        }
        for (uint8_t step = 0; step < definition->step_count; ++step) {
            const m2_step_t *instruction = &definition->steps[step];
            if (instruction->type > M2_STEP_DELAY ||
                (instruction->type == M2_STEP_DELAY &&
                 (instruction->duration_ms == 0 || instruction->duration_ms > 5000)) ||
                ((instruction->type == M2_STEP_KEY_DOWN || instruction->type == M2_STEP_KEY_UP) &&
                 instruction->value == 0) ||
                ((instruction->type == M2_STEP_MOD_DOWN || instruction->type == M2_STEP_MOD_UP) &&
                 (instruction->value == 0 || (instruction->value & (instruction->value - 1)) != 0))) {
                return false;
            }
        }
    }
    for (uint8_t page = 0; page < header->page_count; ++page) {
        for (uint8_t index = 0; index < M2_BUTTONS_PER_PAGE; ++index) {
            const m2_button_t *button = &bundle->buttons[page][index];
            if (button->icon_id >= (sizeof(s_icon_cache) / sizeof(s_icon_cache[0])) ||
                button->action_type > M2_ACTION_PROFILE_NEXT ||
                (button->action_type == M2_ACTION_MACRO && button->action_arg >= header->macro_count)) {
                return false;
            }
        }
    }
    return true;
}

static uint8_t m2_active_modifiers(void)
{
    uint8_t modifiers = 0;
    for (uint8_t bit = 0; bit < 8; ++bit) {
        if (s_modifier_refcount[bit] != 0) {
            modifiers |= (uint8_t) (1U << bit);
        }
    }
    return modifiers;
}

static void m2_emit_keyboard_report(void)
{
    if (!s_usb_mounted || !tud_hid_n_ready(0)) {
        return;
    }
    uint8_t keycodes[6] = {0};
    uint8_t key_count = 0;
    for (uint16_t key = 1; key < 256; ++key) {
        if (s_key_refcount[key] == 0) {
            continue;
        }
        if (key_count == sizeof(keycodes)) {
            ESP_LOGE(TAG, "M2_HID result=too_many_keys action=release_all");
            memset(s_key_refcount, 0, sizeof(s_key_refcount));
            memset(s_modifier_refcount, 0, sizeof(s_modifier_refcount));
            break;
        }
        keycodes[key_count++] = (uint8_t) key;
    }
    tud_hid_keyboard_report(M2_HID_REPORT_ID, m2_active_modifiers(), keycodes);
}

static void m2_slot_release(m2_slot_t *slot)
{
    for (uint16_t key = 1; key < 256; ++key) {
        if (slot->held_keys[key] && s_key_refcount[key] != 0) {
            --s_key_refcount[key];
        }
    }
    for (uint8_t bit = 0; bit < 8; ++bit) {
        if ((slot->held_modifiers & (1U << bit)) && s_modifier_refcount[bit] != 0) {
            --s_modifier_refcount[bit];
        }
    }
    memset(slot, 0, sizeof(*slot));
}

static void m2_engine_release_all(const char *reason)
{
    if (s_engine_lock == NULL || xSemaphoreTake(s_engine_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }
    for (uint8_t slot = 0; slot < M2_MAX_SLOTS; ++slot) {
        m2_slot_release(&s_slots[slot]);
    }
    memset(s_key_refcount, 0, sizeof(s_key_refcount));
    memset(s_modifier_refcount, 0, sizeof(s_modifier_refcount));
    m2_emit_keyboard_report();
    xSemaphoreGive(s_engine_lock);
    ESP_LOGW(TAG, "M2_SAFE_RELEASE reason=%s", reason);
}

static void m2_engine_start(uint8_t button_id, uint8_t macro_id)
{
    if (!s_usb_mounted || macro_id >= s_bundle.header.macro_count ||
        xSemaphoreTake(s_engine_lock, pdMS_TO_TICKS(20)) != pdTRUE) {
        ESP_LOGW(TAG, "M2_MACRO result=rejected button=%u macro=%u usb=%u",
                 button_id, macro_id, s_usb_mounted);
        return;
    }
    // Same physical button restart: release only that slot's held controls.
    for (uint8_t slot = 0; slot < M2_MAX_SLOTS; ++slot) {
        if (s_slots[slot].active && s_slots[slot].owner_button == button_id) {
            m2_slot_release(&s_slots[slot]);
        }
    }
    for (uint8_t slot = 0; slot < M2_MAX_SLOTS; ++slot) {
        if (!s_slots[slot].active) {
            s_slots[slot] = (m2_slot_t) {
                .active = true,
                .owner_button = button_id,
                .macro_id = macro_id,
                .due_us = esp_timer_get_time(),
            };
            xSemaphoreGive(s_engine_lock);
            ESP_LOGI(TAG, "M2_MACRO result=started button=%u macro=%u slot=%u", button_id, macro_id, slot);
            return;
        }
    }
    xSemaphoreGive(s_engine_lock);
    ESP_LOGW(TAG, "M2_MACRO result=slots_full button=%u macro=%u", button_id, macro_id);
}

static void m2_process_slot(m2_slot_t *slot, int64_t now_us)
{
    const m2_macro_t *definition = &s_bundle.macros[slot->macro_id];
    bool changed = false;
    while (slot->active && slot->next_step < definition->step_count && now_us >= slot->due_us) {
        const m2_step_t *instruction = &definition->steps[slot->next_step++];
        if (instruction->type == M2_STEP_DELAY) {
            slot->due_us = now_us + ((int64_t) instruction->duration_ms * 1000);
            break;
        }
        if (instruction->type == M2_STEP_KEY_DOWN && !slot->held_keys[instruction->value]) {
            slot->held_keys[instruction->value] = true;
            ++s_key_refcount[instruction->value];
            changed = true;
        } else if (instruction->type == M2_STEP_KEY_UP && slot->held_keys[instruction->value]) {
            slot->held_keys[instruction->value] = false;
            if (s_key_refcount[instruction->value] != 0) {
                --s_key_refcount[instruction->value];
            }
            changed = true;
        } else if (instruction->type == M2_STEP_MOD_DOWN) {
            const uint8_t bit = (uint8_t) __builtin_ctz(instruction->value);
            if (!(slot->held_modifiers & instruction->value)) {
                slot->held_modifiers |= instruction->value;
                ++s_modifier_refcount[bit];
                changed = true;
            }
        } else if (instruction->type == M2_STEP_MOD_UP) {
            const uint8_t bit = (uint8_t) __builtin_ctz(instruction->value);
            if (slot->held_modifiers & instruction->value) {
                slot->held_modifiers &= (uint8_t) ~instruction->value;
                if (s_modifier_refcount[bit] != 0) {
                    --s_modifier_refcount[bit];
                }
                changed = true;
            }
        }
    }
    if (slot->active && slot->next_step >= definition->step_count) {
        m2_slot_release(slot);
        changed = true;
    }
    if (changed) {
        m2_emit_keyboard_report();
    }
}

static void m2_scheduler_task(void *argument)
{
    (void) argument;
    while (true) {
        if (xSemaphoreTake(s_engine_lock, pdMS_TO_TICKS(2)) == pdTRUE) {
            const int64_t now_us = esp_timer_get_time();
            for (uint8_t slot = 0; slot < M2_MAX_SLOTS; ++slot) {
                if (s_slots[slot].active) {
                    m2_process_slot(&s_slots[slot], now_us);
                }
            }
            xSemaphoreGive(s_engine_lock);
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

static void m2_render_page(void);

static void m2_tile_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    const uint8_t button_id = (uint8_t) (uintptr_t) lv_event_get_user_data(event);
    const m2_button_t *button = &s_bundle.buttons[s_current_page][button_id];
    if (button->action_type == M2_ACTION_MACRO) {
        m2_engine_start(button_id, button->action_arg);
    } else if (button->action_type == M2_ACTION_PAGE_NEXT) {
        s_current_page = (uint8_t) ((s_current_page + 1) % s_bundle.header.page_count);
        ESP_LOGI(TAG, "M2_PAGE action=next page=%u", s_current_page);
        m2_render_page();
    } else if (button->action_type == M2_ACTION_PAGE_PREVIOUS) {
        s_current_page = s_current_page == 0 ? (s_bundle.header.page_count - 1) : (s_current_page - 1);
        ESP_LOGI(TAG, "M2_PAGE action=previous page=%u", s_current_page);
        m2_render_page();
    } else if (button->action_type == M2_ACTION_PROFILE_NEXT) {
        s_current_profile = (uint8_t) ((s_current_profile + 1) % s_bundle.header.profile_count);
        s_current_page = 0;
        ESP_LOGI(TAG, "M2_PROFILE action=next profile=%u", s_current_profile);
        m2_render_page();
    }
}

static void m2_render_page(void)
{
    lv_obj_t *screen = lv_screen_active();
    const int width = lv_display_get_horizontal_resolution(s_display);
    const int height = lv_display_get_vertical_resolution(s_display);
    const int tile_size = (width - (2 * M2_MARGIN_X_PX) - ((M2_COLUMNS - 1) * M2_GAP_PX)) / M2_COLUMNS;
    const int grid_height = (M2_ROWS * tile_size) + ((M2_ROWS - 1) * M2_GAP_PX);
    const int top_bar = (height - grid_height) / 2;

    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    for (uint8_t row = 0; row < M2_ROWS; ++row) {
        for (uint8_t column = 0; column < M2_COLUMNS; ++column) {
            const uint8_t index = row * M2_COLUMNS + column;
            const m2_button_t *definition = &s_bundle.buttons[s_current_page][index];
            const m2_icon_t *icon = &s_icon_cache[definition->icon_id];
            lv_obj_t *tile = lv_button_create(screen);
            lv_obj_set_size(tile, tile_size, tile_size);
            lv_obj_set_pos(tile, M2_MARGIN_X_PX + column * (tile_size + M2_GAP_PX),
                           top_bar + row * (tile_size + M2_GAP_PX));
            lv_obj_set_style_radius(tile, 12, 0);
            lv_obj_set_style_border_width(tile, 0, 0);
            lv_obj_set_style_bg_color(tile, lv_color_hex(icon->color + (s_current_profile * 0x080808)), 0);
            lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(tile, lv_color_hex(0xFFFFFF), LV_STATE_PRESSED);
            lv_obj_set_style_bg_opa(tile, LV_OPA_30, LV_STATE_PRESSED);
            lv_obj_add_event_cb(tile, m2_tile_event_cb, LV_EVENT_CLICKED, (void *) (uintptr_t) index);
            lv_obj_t *symbol = lv_label_create(tile);
            lv_label_set_text(symbol, icon->symbol);
            lv_obj_set_style_text_color(symbol, lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_text_font(symbol, &lv_font_montserrat_14, 0);
            lv_obj_center(symbol);
        }
    }
    ESP_LOGI(TAG, "M2_UI page=%u profile=%u grid=8x4 square_px=%d black_bars_px=%d,%d",
             s_current_page, s_current_profile, tile_size, top_bar, height - grid_height - top_bar);
}

static void m2_usb_event_cb(tinyusb_event_t *event, void *argument)
{
    (void) argument;
    if (event->id == TINYUSB_EVENT_ATTACHED) {
        s_usb_mounted = true;
        ESP_LOGI(TAG, "M2_USB state=mounted interface=boot_keyboard");
    } else if (event->id == TINYUSB_EVENT_DETACHED) {
        s_usb_mounted = false;
        m2_engine_release_all("usb_detached");
        ESP_LOGW(TAG, "M2_USB state=unmounted");
    }
}

static void m2_usb_init(void)
{
    tinyusb_config_t config = TINYUSB_DEFAULT_CONFIG();
    config.descriptor.device = NULL;
    config.descriptor.full_speed_config = s_hid_configuration_descriptor;
    config.descriptor.string = s_hid_string_descriptors;
    config.descriptor.string_count = sizeof(s_hid_string_descriptors) / sizeof(s_hid_string_descriptors[0]);
    config.event_cb = m2_usb_event_cb;
#if (TUD_OPT_HIGH_SPEED)
    config.descriptor.high_speed_config = s_hid_configuration_descriptor;
#endif
    ESP_ERROR_CHECK(tinyusb_driver_install(&config));
    ESP_LOGI(TAG, "M2_USB state=ready keyboard=F13-F24 slots=%u", M2_MAX_SLOTS);
}

void app_main(void)
{
    ESP_LOGI(TAG, "M2_START idf=%s target=esp32p4", esp_get_idf_version());
    m2_build_default_bundle(&s_bundle);
    if (!m2_validate_bundle(&s_bundle)) {
        ESP_LOGE(TAG, "M2_CONFIG result=invalid fallback=disabled_hid");
        return;
    }
    ESP_LOGI(TAG, "M2_CONFIG result=accepted version=%u pages=%u profiles=%u macros=%u",
             s_bundle.header.version, s_bundle.header.page_count,
             s_bundle.header.profile_count, s_bundle.header.macro_count);

    s_engine_lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(s_engine_lock == NULL ? ESP_ERR_NO_MEM : ESP_OK);
    m2_engine_release_all("boot");
    ESP_ERROR_CHECK(xTaskCreate(m2_scheduler_task, "m2_scheduler", 4096, NULL, 6, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    m2_usb_init();

    bsp_display_cfg_t display_cfg = {
        .lv_adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG(),
        .rotation = ESP_LV_ADAPTER_ROTATE_90,
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_PARTIAL,
        .touch_flags = {
            .swap_xy = true,
            .mirror_x = true,
            .mirror_y = false,
        },
    };
    s_display = bsp_display_start_with_config(&display_cfg);
    if (s_display == NULL) {
        ESP_LOGE(TAG, "M2_UI result=display_start_failed");
        m2_engine_release_all("display_start_failed");
        return;
    }
    bsp_display_backlight_on();
    ESP_ERROR_CHECK(esp_lv_adapter_lock(UINT32_MAX));
    m2_render_page();
    esp_lv_adapter_unlock();

    ESP_LOGI(TAG, "M2_COMPLETE ui=ready scheduler_slots=%u recovery=release_all", M2_MAX_SLOTS);
}
