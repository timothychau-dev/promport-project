// main/apps/presenter/app_presenter.cc — Presenter (BLE HID slide clicker).
//
// Passport OS app wrapper around the proven Bluedroid esp_hid keyboard port
// (apps/presenter/ble_hid.c, pattern: official esp_hid_device example).
//
// Keys (global "long OK = back" rule respected, so Exit moved to DOWN long):
//   UP short  -> Left Arrow  (previous slide)
//   DOWN short-> Right Arrow (next slide)
//   OK  short -> start slideshow (F5 / Cmd+Shift+Return / Opt+Cmd+P) + talk timer
//   OK  long  -> handled by app_registry (back to launcher)
//   UP  long  -> arm pairing reset; DOWN long within 3s wipes BLE bonding
//   DOWN long -> Esc (exit slideshow + stop timer), unless reset is armed
// BLE runs only while this app is in the foreground (init on start, adv stop
// on leave; bonding keys persist in NVS).

#include "apps/app_base.h"
#include "ble_hid.h"
#include "launcher/app_registry.h"
#include "bsp_display.h"
#include "bsp_battery.h"
#include "wifi_manager.h"
#include "app_fonts.h"
#include "ui_pixel.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstdio>
#include <cstring>

namespace {

lv_obj_t *s_screen;
lv_timer_t *s_timer;
lv_obj_t *s_status, *s_timer_label, *s_action;
lv_obj_t *s_wifi_bars[3] = {nullptr, nullptr, nullptr};
static lv_obj_t *s_status_clock;
ui_pixel_battery_t s_battery{};
uint64_t s_battery_updated_us;

bool s_running = false;
uint32_t s_seconds = 0;
uint64_t s_started_us = 0;

// Pairing-reset arm window (single ADC ladder cannot detect two-key combos).
#define RESET_ARM_WINDOW_MS 3000
uint64_t s_reset_arm_us = 0;  // 0 = not armed

lv_obj_t *label(const char *text, const lv_font_t *font, uint32_t color,
                int x, int y, int w, int h) {
    lv_obj_t *o = lv_label_create(s_screen);
    lv_label_set_text(o, text);
    lv_obj_set_style_text_font(o, font, 0);
    lv_obj_set_style_text_color(o, lv_color_hex(color), 0);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_label_set_long_mode(o, LV_LABEL_LONG_DOT);
    return o;
}
void set_text(lv_obj_t *obj, const char *text) {
    if (obj && std::strcmp(lv_label_get_text(obj), text) != 0) lv_label_set_text(obj, text);
}
void set_action(const char *text, uint32_t color) {
    if (!s_action) return;
    ui_pixel_label_set_text(s_action, text);
    ui_pixel_text_color(s_action, color);
}
void timer_refresh() {
    if (!s_timer_label) return;
    char buf[16];
    if (s_running && s_started_us) {
        s_seconds = static_cast<uint32_t>((esp_timer_get_time() - s_started_us) / 1000000);
    }
    std::snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu",
                  static_cast<unsigned long>(s_seconds / 3600),
                  static_cast<unsigned long>((s_seconds % 3600) / 60),
                  static_cast<unsigned long>(s_seconds % 60));
    set_text(s_timer_label, buf);
}
void status_refresh() {
    if (!s_status) return;
    char peer[20], buf[64];
    if (ble_hid_is_connected()) {
        const bool have_peer = ble_hid_get_peer_str(peer, sizeof(peer));
        std::snprintf(buf, sizeof(buf), "Connected%s%s", have_peer ? "\n" : "",
                      have_peer ? peer : "");
        ui_pixel_text_color(s_status, UI_ACCENT);
    } else {
        std::snprintf(buf, sizeof(buf), "Pair with\nPrompter");
        ui_pixel_text_color(s_status, UI_TEXT_DIM);
    }
    set_text(s_status, buf);
    if (!s_battery_updated_us || esp_timer_get_time() - s_battery_updated_us >= 5000000) {
        ui_pixel_battery_set(&s_battery, bsp_battery_soc());
        ui_pixel_status_clock_set(s_status_clock);
        const bool wifi_ok = WifiManager::GetInstance().IsConnected();
        int bars = 0;
        uint32_t wifi_col = UI_TEXT_DIM;
        if (wifi_ok) {
            const int rssi = WifiManager::GetInstance().GetRssi();
            bars = rssi >= -60 ? 3 : (rssi >= -75 ? 2 : 1);
            wifi_col = UI_TEXT;
        }
        ui_pixel_wifi_bars_set(s_wifi_bars, bars, wifi_col);
        s_battery_updated_us = esp_timer_get_time();
    }
}
void refresh() {
    if (!s_screen) return;
    status_refresh();
    timer_refresh();
    ui_pixel_text_color(s_timer_label, s_running ? UI_ACCENT : UI_TEXT);
}
void tick(lv_timer_t *timer) {
    const bool visible = app_registry_screen_is_on();
    lv_timer_set_period(timer, visible && s_running ? 1000 : 2000);
    if (visible) refresh();
}
void init() {}
void start() {
    if (!bsp_lvgl_lock(1000)) return;
    if (s_screen) { bsp_lvgl_unlock(); return; }
    s_screen = lv_obj_create(nullptr);
    ui_pixel_background(s_screen);
    lv_obj_t *top = ui_pixel_status_bar(s_screen, UI_TEXT, &s_battery, s_wifi_bars, &s_status_clock);
    s_status = label("", &buddy_font_16, UI_TEXT_DIM, 12, 50, 216, 44);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, 0);
    s_timer_label = label("00:00:00", &lv_font_montserrat_32,
        ui_theme_is_multicolor() ? UI_SECONDARY : UI_TEXT, 12, 108, 216, 46);
    lv_obj_set_style_text_align(s_timer_label, LV_TEXT_ALIGN_CENTER, 0);
    s_action = label("Ready", &buddy_font_16, UI_ACCENT, 12, 170, 216, 24);
    lv_obj_set_style_text_align(s_action, LV_TEXT_ALIGN_CENTER, 0);
    s_battery_updated_us = 0;
    s_reset_arm_us = 0;
    refresh();
    lv_screen_load(s_screen);
    s_timer = lv_timer_create(tick, 2000, nullptr);
    bsp_lvgl_unlock();

    // BLE only while the page is open; bonding survives in NVS.
    ble_hid_init();
    ESP_LOGI("presenter", "started; BLE HID advertising");
}
void stop() {
    ble_hid_stop();
    if (!bsp_lvgl_lock(-1)) return;
    if (s_timer) { lv_timer_delete(s_timer); s_timer = nullptr; }
    if (s_screen) { lv_obj_delete(s_screen); s_screen = nullptr; }
    s_wifi_bars[0] = s_wifi_bars[1] = s_wifi_bars[2] = nullptr;
    s_battery = {};
    s_action = nullptr;
    bsp_lvgl_unlock();
    ESP_LOGI("presenter", "stopped; BLE advertising off");
}
void on_key(uint8_t btn, app_btn_event_t event, uint16_t) {
    if (event == BTN_EVT_LONG && btn == BSP_BTN_UP) {
        // Arm the pairing reset; any other key disarms it.
        if (!bsp_lvgl_lock(500)) return;
        s_reset_arm_us = esp_timer_get_time();
        set_action("RESET: hold DOWN", UI_ORANGE);
        bsp_lvgl_unlock();
        ESP_LOGI("presenter", "reset armed; hold DOWN within 3s");
        return;
    }
    if (event == BTN_EVT_LONG && btn == BSP_BTN_DOWN) {
        const bool armed = s_reset_arm_us != 0 &&
            esp_timer_get_time() - s_reset_arm_us <= RESET_ARM_WINDOW_MS * 1000;
        s_reset_arm_us = 0;
        if (armed) {
            if (!bsp_lvgl_lock(500)) return;
            set_action("RESETTING...", UI_ORANGE);
            bsp_lvgl_unlock();
            vTaskDelay(pdMS_TO_TICKS(500));  // let the frame land before restart
            ble_hid_reset_bonding();
            return;
        }
        // Esc: exit slideshow + stop the talk timer.
        if (!bsp_lvgl_lock(500)) return;
        ble_hid_key_press(HID_KEY_ESCAPE);
        s_running = false;
        s_started_us = 0;
        s_seconds = 0;
        set_action("EXIT", UI_ORANGE);
        timer_refresh();
        ui_pixel_text_color(s_timer_label, UI_TEXT);
        bsp_lvgl_unlock();
        ESP_LOGI("presenter", "esc; timer stopped & reset");
        return;
    }
    if (event != BTN_EVT_CLICK) return;
    if (!bsp_lvgl_lock(500)) return;
    if (s_screen) {
        if (btn == BSP_BTN_UP) {
            ble_hid_key_press(HID_KEY_LEFT_ARROW);
            set_action("<< Prev", UI_ACCENT);
            ESP_LOGI("presenter", "UP -> left arrow");
        } else if (btn == BSP_BTN_DOWN) {
            ble_hid_key_press(HID_KEY_RIGHT_ARROW);
            set_action("Next >>", UI_ACCENT);
            ESP_LOGI("presenter", "DOWN -> right arrow");
        } else if (btn == BSP_BTN_OK) {
            ble_hid_press_start_slideshow();
            if (!s_running) {
                s_running = true;
                s_started_us = esp_timer_get_time();
                s_seconds = 0;
            }
            set_action("START", UI_ACCENT);
            ESP_LOGI("presenter", "OK -> start slideshow; timer running");
        }
        timer_refresh();
        if (s_timer) lv_timer_set_period(s_timer, s_running ? 1000 : 2000);
    }
    bsp_lvgl_unlock();
}
} // namespace

extern const passport_app_t g_presenter_app = {
    .id = APP_ID_PRESENTER,
    .name = "Prompter",
    .en_name = "PRESENTER",
    .desc = "BLE slide clicker + talk timer\nPair with Prompter",
    .tag = "BLE",
    .theme_color = 0x6C8DDB,
    .init = init,
    .start = start,
    .stop = stop,
    .on_key = on_key,
};
