#pragma once
#include "pomodoro_platform.h"
static inline lv_obj_t *ui_pixel_status_bar(lv_obj_t *p, uint32_t, ui_pixel_battery_t *, lv_obj_t *bars[3], lv_obj_t **clock) {
    if (bars) for (int i = 0; i < 3; ++i) bars[i] = nullptr;
    if (clock) *clock = nullptr;
    return p;
}
static inline void ui_pixel_status_clock_set(lv_obj_t *) {}
static inline void ui_pixel_wifi_bars_set(lv_obj_t *[3], int, uint32_t) {}
