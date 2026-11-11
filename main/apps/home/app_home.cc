// 首页(个人主页)应用。
//
// 小内存约束下的三条硬规矩(改动时请保持):
//   1. 头像索引静态存放；显示缓冲仅在首页分配，离开首页即释放;
//   2. 文件 IO(读头像)与网络 IO(拉天气)一律在**不持 LVGL 锁**时完成;
//   3. stop() 必须删屏,与 Launcher 的 launcher_ui_hide() 同理,C3 内部堆没有余量
//      同时容纳两个应用的界面。
//
// 2026-09-13 视觉精致化(全部为"对象拼画 + 低频动画",无 canvas/图片资源):
//   - 昼夜主题:任务里每小时核对,19 点~次日 6 点切夜景(ui_pixel 主题系统);
//   - 天气卡左侧画像素天气图标(晴/少云/阴/雾/雨/雪/雷),拉取中显示四点 spinner;
//   - 无头像空态换全机吉祥物(会眨眼的像素小电视),替代灰色剪影;
//   - 底部白字加墨色错位硬阴影,压在草地上也清晰。
//
// 2026-09-13 晚 · 首页版式重构(上中下三层,用户定稿):
//   顶部:像素块大时钟(5×7 字模拼块,真·像素粗体,直接画在天空上)+ 日期行;
//   中部:组合卡 = 左头像(橙框) + 右侧 天气图标/短语 + 城市 + 温度 + 湿度·风级一行
//        (原两张统计卡降维合并进这一行);
//   底部:名牌卡 = 姓名(20px 标题字) + 个性签名,浮在天空上,草地只留按键提示。
//
// 2026-09-13 深夜 · 绿磷光锁屏改版(参照用户提供的复古终端风参考图):
//   整页黑底 + 磷光绿单色,三列布局:
//   左卡(104×202):人像区(96×140,头像经亮度阈值转 A8 单色) + 姓名 + 签名;
//   右列(120 宽):点阵时钟卡(5×7 字模,每格画 4×4 光点) / 日期+城市+Wi-Fi 卡 /
//                天气卡(单色图标 + 温度 + 短语 + 分隔线 + 风向·风级);
//   底部:操作提示与 A8 艺术底图已按产品要求移除,上传模式仅显示访问地址。
//   美术显示走 LVGL A8 快速路径(alpha 蒙版 + image_recolor 纯色),已核对
//   lv_draw_sw_img.c:233 与 lv_draw_sw_blend_to_rgb565.c。
#include "apps/home/app_home.h"

#include "apps/home/home_clock.h"
#include "apps/home/home_profile.h"
#include "apps/home/home_upload.h"
#include "apps/home/home_weather.h"
#include "common/time_sync.h"
#include "launcher/app_registry.h"
#include "bsp_display.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "src/libs/tjpgd/tjpgd.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include "fonts/app_fonts.h"   // buddy_font_16:首页中文动态文本全靠它
#include "ui_pixel.h"
#include "wifi_manager.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

extern "C" {
#include "bsp_battery.h"
}

#include <algorithm>
#include <ctime>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

constexpr char kTag[] = "app_home";
LV_FONT_DECLARE(lv_font_unscii_8);

// A8 artwork encodes highlights: keep a dark home scene even in the light theme.
#define kBg (ui_theme_color_on_dark(UI_COLOR_BG))
#define kPhos (ui_theme_color_on_dark(UI_COLOR_TEXT))
#define kPhosDim (ui_theme_color_on_dark(UI_COLOR_TEXT_DIM))
#define kPhosBorder (ui_theme_color_on_dark(UI_COLOR_BORDER))
#define kClock (ui_theme_get() == UI_THEME_NEON ? UI_ACCENT : kPhos)
#define kTemperature (ui_theme_is_multicolor() ? ui_theme_color_on_dark(UI_COLOR_TERTIARY) : kPhos)
#define kWeatherAccent (ui_theme_color_on_dark(ui_theme_get() == UI_THEME_NEON ? UI_COLOR_SUCCESS : UI_COLOR_ACCENT))

// 兼容既有更新函数的语义色:磷光风格里"强调/在线"都是亮绿,弱化都是暗绿。
#define kText (kPhos)
#define kSub (kPhosDim)
#define kMuted (kPhosDim)
#define kAccent (kPhos)
#define kOnline (kPhos)
#define kOffline (kPhosDim)

// 版式栅格:所有元素都挂在这几个常量上,改一个边距整屏跟着走,不再散落硬编码。
constexpr int kMargin = 12;
constexpr int kContentW = 240 - kMargin * 2;  // 216

constexpr int64_t kWeatherFirstDelayMs = 15000;
constexpr int64_t kWeatherPeriodMs = 1800000;
// 失败(信号差/热点刚连上)后 2 分钟再试,而不是干等 30 分钟。
constexpr int64_t kWeatherRetryMs = 120000;
// 超过 2 小时的天气在风级行改为显示"几小时前更新",户外断网时不把旧数据当实时。
constexpr int64_t kWeatherStaleSec = 7200;
constexpr int64_t kUploadTimeoutMs = 600000;

// Only the Home worker owns this state. Keep deadlines across app visits so a
// fresh sample does not cause another pair of TLS requests on every return.
struct WeatherSchedule {
    static constexpr unsigned Weather = 1, Air = 2, Both = Weather | Air;
    int64_t weather_due_ms = -1, air_due_ms = -1;
    char lat[HOME_COORD_MAX]{}, lon[HOME_COORD_MAX]{};

    unsigned due(int64_t now, int64_t entered, bool force, const home_profile_t &profile) const {
        if (!profile.lat[0] || !profile.lon[0]) return 0;
        if (force || (lat[0] && (std::strcmp(lat, profile.lat) || std::strcmp(lon, profile.lon)))) return Both;
        if (weather_due_ms < 0 && air_due_ms < 0) return now - entered >= kWeatherFirstDelayMs ? Both : 0;
        return (weather_due_ms <= now ? Weather : 0u) | (air_due_ms <= now ? Air : 0u);
    }

    void begin(unsigned requests, int64_t now, const home_profile_t &profile) {
        std::snprintf(lat, sizeof(lat), "%s", profile.lat);
        std::snprintf(lon, sizeof(lon), "%s", profile.lon);
        // An interrupted request stays due when the next Home worker starts.
        if (requests & Weather) weather_due_ms = now;
        if (requests & Air) air_due_ms = now;
    }

    void finish(unsigned request, int64_t now, esp_err_t result) {
        const int64_t next = now + (request == Weather && result != ESP_OK ? kWeatherRetryMs : kWeatherPeriodMs);
        if (request == Weather) weather_due_ms = next;
        else air_due_ms = next;
    }
};
WeatherSchedule s_weather_schedule;

// Packed two-bit tone indices; the theme supplies colors at display time.
// Home owns this buffer only while its screen/worker are alive.
uint8_t *s_portrait_packed = nullptr;

esp_err_t read_portrait(bool present) {
    if (!present) return ESP_FAIL;
    if (!s_portrait_packed)
        s_portrait_packed = static_cast<uint8_t *>(malloc(HOME_AVATAR_BYTES));
    if (!s_portrait_packed) return ESP_ERR_NO_MEM;
    return home_profile_read_avatar(s_portrait_packed, HOME_AVATAR_BYTES);
}
// 显示用 A8 展开:LVGL 软渲染 A8 快速路径(alpha 蒙版 + recolor 磷光绿纯色)。
// 2026-09-13 改为堆分配(home_start 申请 / home_stop 归还):14.4KB 常驻 .bss
// 会直接从堆里扣掉,而小智等应用恰恰在这块内存上跑 Opus 编码器 —— 之前
// largest 只剩 22KB,编码器(需约 25KB 连续块)必然 open 失败。首页与其它
// 应用互斥运行,切走时归还,谁在用谁付钱。
uint8_t *s_portrait_pixels = nullptr;
lv_image_dsc_t s_portrait_dsc;

std::atomic<bool> s_home_active{false};
// 界面存活开关:home_task 每轮持锁刷新前检查,stop() 删屏前置 false。
// 解决"stop 超时后任务仍活着却去写已删除 label"这一类的 use-after-free。
std::atomic<bool> s_ui_alive{false};
std::atomic<bool> s_force_weather{false};
TaskHandle_t s_task_handle = nullptr;
std::atomic<bool> s_worker_running{false};

lv_obj_t *s_screen = nullptr;
lv_obj_t *s_avatar_img = nullptr;      // Indexed portrait with a transparent background.
lv_obj_t *s_portrait_ph = nullptr;     // 无头像占位文字
lv_obj_t *s_wifi_bars[3] = {nullptr, nullptr, nullptr};
ui_pixel_battery_t s_batt = {nullptr, nullptr};
bool s_share_visible = false;
uint8_t s_share_index = 0;

lv_obj_t *s_lbl_name = nullptr;
lv_obj_t *s_lbl_sig = nullptr;
lv_obj_t *s_lbl_clock = nullptr;        // 统一状态栏时间(仅 Wi-Fi/时间/电池三项)


lv_obj_t *s_lbl_upload = nullptr;       // 上传模式时显示手机访问地址(唯一保留的提示文字)

void expand_portrait(void) {
    if (!s_portrait_pixels || !s_portrait_packed) return;
    lv_image_cache_drop(&s_portrait_dsc);
    if (s_portrait_packed[4] == 0) {
        // Preserve LVGL's original RGB565 alpha blending, including its rounding.
        for (size_t pixel = 0; pixel < HOME_PORTRAIT_PIXELS; ++pixel)
            s_portrait_pixels[pixel] = home_portrait_index(s_portrait_packed, pixel) * 85;
        return;
    }
    memset(s_portrait_pixels, 0, HOME_PORTRAIT_I4_BYTES);
    auto *palette = reinterpret_cast<lv_color32_t *>(s_portrait_pixels);
    for (uint8_t i = 1; i <= 4; ++i)
        palette[i] = lv_color_to_32(lv_color_hex(ui_theme_portrait_subject_color(i - 1)), LV_OPA_COVER);
    for (size_t pixel = 0; pixel < HOME_PORTRAIT_PIXELS; pixel += 2)
        s_portrait_pixels[64 + pixel / 2] = (home_portrait_index(s_portrait_packed, pixel) << 4) |
            home_portrait_index(s_portrait_packed, pixel + 1);
}

// 无头像时显示占位文字(磷光风格不用彩色吉祥物)。
void apply_avatar(bool have_image) {
    have_image = have_image && s_portrait_packed;
    const bool legacy = have_image && s_portrait_packed[4] == 0;
    const size_t bytes = legacy ? HOME_PORTRAIT_PIXELS : HOME_PORTRAIT_I4_BYTES;
    if (have_image && s_portrait_pixels && s_portrait_dsc.data_size != bytes) {
        lv_image_set_src(s_avatar_img, nullptr);
        lv_image_cache_drop(&s_portrait_dsc);
        free(s_portrait_pixels);
        s_portrait_pixels = nullptr;
    }
    if (have_image && !s_portrait_pixels) {
        s_portrait_pixels = static_cast<uint8_t *>(malloc(bytes));
        s_portrait_dsc.data = s_portrait_pixels;
        if (!s_portrait_pixels) ESP_LOGW(kTag, "portrait buffer unavailable; using placeholder");
    }
    if (have_image && s_portrait_pixels != nullptr) {
        s_portrait_dsc.header.cf = legacy ? LV_COLOR_FORMAT_A8 : LV_COLOR_FORMAT_I4;
        s_portrait_dsc.header.stride = legacy ? HOME_PORTRAIT_W : HOME_PORTRAIT_W / 2;  // 均按 96 宽
        s_portrait_dsc.data_size = bytes;
        lv_obj_set_style_image_recolor(s_avatar_img, lv_color_hex(ui_theme_portrait_color(3)), 0);
        expand_portrait();
        lv_image_set_src(s_avatar_img, &s_portrait_dsc);
        lv_obj_clear_flag(s_avatar_img, LV_OBJ_FLAG_HIDDEN);
        if (s_portrait_ph) lv_obj_add_flag(s_portrait_ph, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_image_set_src(s_avatar_img, nullptr);
        lv_obj_add_flag(s_avatar_img, LV_OBJ_FLAG_HIDDEN);
        if (s_portrait_ph) lv_obj_clear_flag(s_portrait_ph, LV_OBJ_FLAG_HIDDEN);
    }
}

// 电量画成一个 24×12 的迷你电池:外框 + 按 SOC 伸缩的内条 + 右侧百分比数字。
// 顶栏电池与 Wi-Fi 信号都用共享像素构件,与 Launcher 顶栏一致。
void update_battery_label() {
    ui_pixel_status_clock_set(s_lbl_clock);
    ui_pixel_battery_set_on_dark(&s_batt, bsp_battery_soc());

    const bool connected = WifiManager::GetInstance().IsConnected();
    int bars = 0;
    uint32_t color = kOffline;
    if (connected) {
        const int rssi = WifiManager::GetInstance().GetRssi();
        bars = rssi >= -60 ? 3 : (rssi >= -75 ? 2 : 1);
        color = kOnline;
    }
    ui_pixel_wifi_bars_set(s_wifi_bars, bars, color);
}

constexpr uint16_t kJpegMcuHeight = 16;
constexpr std::size_t kJpegWorkSize = 4096;

struct ShareJpegContext {
    FILE *fp;
    uint16_t *stripe;
    uint16_t width;
    uint16_t height;
    bool color_pending;
    bool failed;
};

std::size_t share_jpeg_input(JDEC *decoder, uint8_t *buffer, std::size_t requested) {
    auto *context = static_cast<ShareJpegContext *>(decoder->device);
    if (!context || !context->fp) return 0;
    if (buffer) {
        return std::fread(buffer, 1, requested, context->fp);
    } else {
        return std::fseek(context->fp, static_cast<long>(requested), SEEK_CUR) == 0 ? requested : 0;
    }
}

int share_jpeg_output(JDEC *decoder, void *bitmap, JRECT *rectangle) {
    auto *context = static_cast<ShareJpegContext *>(decoder->device);
    if (!context || !bitmap || !rectangle || context->failed) return 0;

    const uint16_t block_width = rectangle->right - rectangle->left + 1;
    const uint16_t block_height = rectangle->bottom - rectangle->top + 1;
    if (rectangle->right >= context->width || rectangle->bottom >= context->height) {
        context->failed = true;
        return 0;
    }

    if (rectangle->left == 0 && context->color_pending) {
        if (bsp_display_io()) {
            esp_lcd_panel_io_tx_param(bsp_display_io(), 0x00, nullptr, 0);
        }
        context->color_pending = false;
    }

    uint16_t *target = context->stripe;
    if (!target) {
        context->failed = true;
        return 0;
    }

    // TJPGD 输出格式为 BGR888
    const auto *bgr = static_cast<const uint8_t *>(bitmap);
    for (uint16_t y = 0; y < block_height; ++y) {
        uint16_t *row = target + static_cast<std::size_t>(y) * context->width;
        for (uint16_t x = 0; x < block_width; ++x) {
            const std::size_t input = (static_cast<std::size_t>(y) * block_width + x) * 3U;
            const uint16_t color = static_cast<uint16_t>(
                ((bgr[input + 2] & 0xf8U) << 8U) |
                ((bgr[input + 1] & 0xfcU) << 3U) |
                (bgr[input] >> 3U));
            row[rectangle->left + x] = __builtin_bswap16(color);
        }
    }

    if (rectangle->right + 1U == context->width) {
        const uint16_t panel_top = rectangle->top;
        const uint16_t panel_bottom = rectangle->bottom + 1U;
        if (esp_lcd_panel_draw_bitmap(bsp_display_panel(), 0, panel_top,
                                      context->width, panel_bottom, target) != ESP_OK) {
            context->failed = true;
            return 0;
        }
        context->color_pending = true;
    }
    return 1;
}

lv_obj_t *px_container(lv_obj_t *parent, int x, int y, int w, int h) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    return o;
}

lv_obj_t *px_block(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    return o;
}

// 静态操作提示与轮播详情已按产品要求移除;唯一保留的文字提示是
// 上传模式的手机访问地址 —— 那一刻用户正等着输入 URL,必须可见。
void update_upload_label() {
    const bool upload = home_upload_active();
    if (!upload && (!s_lbl_upload || lv_obj_has_flag(s_lbl_upload, LV_OBJ_FLAG_HIDDEN))) return;
    if (!s_lbl_upload) return;
    if (upload) {
        ui_pixel_label_set_text(s_lbl_upload, home_upload_url());
        lv_obj_clear_flag(s_lbl_upload, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_lbl_upload, LV_OBJ_FLAG_HIDDEN);
    }
}

void apply_profile(const home_profile_t *prof) {
    const char *name = prof->name[0] ? prof->name : "Name not set";
    lv_label_set_text(s_lbl_name, name);
    // Bebas Neue 只有拉丁字形:全拉丁名字用展示体,其余(含中文)回退 ui_font_20。
    const bool latin_only = [&] {
        for (const char *p = name; *p; ++p)
            if (static_cast<unsigned char>(*p) >= 0x80) return false;
        return true;
    }();
    lv_obj_set_style_text_font(s_lbl_name, latin_only ? &bebas_font_32 : &ui_font_20, 0);
    const char *sig = prof->signature[0] ? prof->signature
                                         : "Don't prompt alone, prompt together.";
    lv_label_set_text(s_lbl_sig, sig);
    // 拉丁签名用原生 Montserrat 14(原尺寸最清晰);含中文回退 buddy_font_16 缩放 0.78×。
    const bool sig_latin = [&] {
        for (const char *p = sig; *p; ++p)
            if (static_cast<unsigned char>(*p) >= 0x80) return false;
        return true;
    }();
    lv_obj_set_style_text_font(s_lbl_sig, sig_latin ? &lv_font_montserrat_14 : &buddy_font_16, 0);
    lv_obj_set_style_transform_scale(s_lbl_sig, sig_latin ? 256 : 199, 0);
}

lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color,
                     lv_text_align_t align) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(label, align, 0);
    return label;
}

void create_ui() {
    s_screen = lv_obj_create(nullptr);
    lv_obj_set_style_pad_all(s_screen, 0, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(kBg), 0);
    lv_obj_set_style_border_width(s_screen, 0, 0);

    // ---- 状态栏 (y0..24):Wi-Fi 信号 + 时间 + 电池,与各场景统一 ----
    ui_pixel_status_bar(s_screen, kPhos, &s_batt, s_wifi_bars, &s_lbl_clock);

    // ---- 主区:头像 128×184 居中(96:138 同比例放大 4/3),姓名 + 签名居中在下 ----
    lv_obj_t *zone = px_container(s_screen, (240 - HOME_PORTRAIT_W) / 2, 46,
                                  HOME_PORTRAIT_W, HOME_PORTRAIT_H);
    lv_obj_set_style_border_width(zone, 0, 0);
    lv_obj_set_style_bg_opa(zone, LV_OPA_TRANSP, 0);

    s_avatar_img = lv_image_create(zone);
    lv_obj_set_pos(s_avatar_img, 0, 0);
    lv_obj_set_size(s_avatar_img, HOME_PORTRAIT_W, HOME_PORTRAIT_H);
    lv_image_set_inner_align(s_avatar_img, LV_IMAGE_ALIGN_CENTER);
    // A8 alpha 图以 recolor 色实心填充(LVGL 软渲染快速路径,已核对 lv_draw_sw_img.c)
    lv_obj_set_style_image_recolor_opa(s_avatar_img, LV_OPA_TRANSP, 0);

    s_portrait_ph = make_label(zone, &buddy_font_16, kPhosDim, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_width(s_portrait_ph, 120);
    lv_obj_align(s_portrait_ph, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(s_portrait_ph, "No avatar");

    s_lbl_name = make_label(s_screen, &ui_font_20, kPhos, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_style_text_letter_space(s_lbl_name, 1, 0);
    lv_obj_set_size(s_lbl_name, kContentW, 34);
    lv_label_set_long_mode(s_lbl_name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(s_lbl_name, kMargin, 241);

    // 签名:默认 Bebas 0.5×(≈16px 展示体);含中文时回退 buddy_font_16 0.78×。
    // 加粗:先铺一层 1px 偏移的同文字标签(在底层),再放正文 —— LVGL 无描边样式。
    // 签名缩至 ~12.5px(0.78×):buddy_font_16 是唯一全 CJK 字库。
    s_lbl_sig = make_label(s_screen, &lv_font_montserrat_14, kPhosDim, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_size(s_lbl_sig, kContentW, 40);
    lv_label_set_long_mode(s_lbl_sig, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_pos(s_lbl_sig, kMargin, 273);
    lv_obj_set_style_transform_scale(s_lbl_sig, 256, 0);
    lv_obj_set_style_transform_pivot_x(s_lbl_sig, kContentW / 2, 0);
    lv_obj_set_style_transform_pivot_y(s_lbl_sig, 0, 0);
    lv_label_set_text(s_lbl_name, "Name not set");
    lv_label_set_text(s_lbl_sig, "Don't prompt alone, prompt together.");

    // ---- 上传地址:平时隐藏,仅上传模式显示 ----
    s_lbl_upload = make_label(s_screen, &lv_font_montserrat_14, kPhos, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_width(s_lbl_upload, kContentW);
    lv_obj_align(s_lbl_upload, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_label_set_long_mode(s_lbl_upload, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_add_flag(s_lbl_upload, LV_OBJ_FLAG_HIDDEN);
}

bool home_show_share_image() {
    char path[HOME_SHARE_PATH_MAX];
    if (!home_profile_share_path_at(s_share_index, path, sizeof(path))) return false;
    FILE *fp = std::fopen(path, "rb");
    if (!fp) {
        ESP_LOGW(kTag, "Cannot open share image: %s", path);
        return false;
    }

    auto *work = static_cast<uint8_t *>(std::malloc(kJpegWorkSize));
    auto *stripe = static_cast<uint16_t *>(
        std::malloc(HOME_SHARE_W * kJpegMcuHeight * sizeof(uint16_t)));
    if (!work || !stripe) {
        ESP_LOGE(kTag, "Alloc failed for share JPEG decoding");
        if (work) std::free(work);
        if (stripe) std::free(stripe);
        std::fclose(fp);
        return false;
    }

    ShareJpegContext context = {
        .fp = fp,
        .stripe = stripe,
        .width = HOME_SHARE_W,
        .height = HOME_SHARE_H,
        .color_pending = false,
        .failed = false,
    };

    JDEC decoder;
    JRESULT res = jd_prepare(&decoder, share_jpeg_input, work, kJpegWorkSize, &context);
    if (res != JDR_OK) {
        ESP_LOGW(kTag, "jd_prepare failed: %d", res);
        std::free(work);
        std::free(stripe);
        std::fclose(fp);
        return false;
    }

    if (decoder.width > HOME_SHARE_W || decoder.height > HOME_SHARE_H) {
        ESP_LOGW(kTag, "Share image dimensions (%ux%u) exceed max (%ux%u)",
                 decoder.width, decoder.height, HOME_SHARE_W, HOME_SHARE_H);
        std::free(work);
        std::free(stripe);
        std::fclose(fp);
        return false;
    }
    context.width = decoder.width;
    context.height = decoder.height;

    res = jd_decomp(&decoder, share_jpeg_output, 0);
    std::fclose(fp);

    if (context.color_pending && bsp_display_io()) {
        esp_lcd_panel_io_tx_param(bsp_display_io(), 0x00, nullptr, 0);
    }

    std::free(work);
    std::free(stripe);

    if (res != JDR_OK || context.failed) {
        ESP_LOGW(kTag, "jd_decomp failed: %d (failed=%d)", res, context.failed);
        return false;
    }

    ESP_LOGI(kTag, "Wallet card displayed: %ux%u color JPEG index=%u free=%u largest=%u",
        decoder.width, decoder.height, s_share_index, (unsigned)esp_get_free_heap_size(),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    return true;
}

void toggle_share_view() {
    if (s_share_visible) {
        bsp_lvgl_lock(-1);
        s_share_visible = false;
        lv_display_t *disp = lv_display_get_default();
        if (disp) lv_display_enable_invalidation(disp, true);
        if (s_screen) lv_obj_invalidate(s_screen);
        bsp_lvgl_unlock();
        return;
    }

    if (!home_profile_has_share_image()) {
        ESP_LOGW(kTag, "UP: no valid wallet card file");
        // 尚未上传随身名片时保留原有的手动天气刷新能力。
        s_force_weather.store(true);
        return;
    }

    bsp_lvgl_lock(-1);
    s_share_index = 0;
    s_share_visible = true;
    lv_display_t *disp = lv_display_get_default();
    if (disp) lv_display_enable_invalidation(disp, false);
    bsp_lvgl_unlock();

    // 在锁外执行文件读取与分块直推屏幕
    if (!home_show_share_image()) {
        bsp_lvgl_lock(-1);
        s_share_visible = false;
        if (disp) lv_display_enable_invalidation(disp, true);
        if (s_screen) lv_obj_invalidate(s_screen);
        bsp_lvgl_unlock();
    }
}

void home_task(void *arg) {
    (void)arg;
    const int64_t start_ms = esp_timer_get_time() / 1000;
    int64_t upload_started_ms = 0;
    bool upload_ps_high = false; // 上传模式是否已切 BALANCED 省电档
    bool clock_on = false;
    uint32_t last_profile_ver = home_profile_version();
    home_profile_t prof;
    home_profile_get(&prof);

    while (s_home_active.load()) {
        const int64_t now_ms = esp_timer_get_time() / 1000;

        if (bsp_lvgl_lock(1000)) {
            if (s_ui_alive.load() && app_registry_screen_is_on() && !s_share_visible) {
                update_battery_label();
                // 上传模式下每秒重算:把刚拿到的 IP 补进 URL,
                // 避免"Wi-Fi 刚连上还没拿到地址"时显示成空的 http://。
                update_upload_label();
            }
            bsp_lvgl_unlock();
        }

        // Wi-Fi 就绪才跑 SNTP;断开即停,避免 lwIP 一直重试。
        const bool connected = WifiManager::GetInstance().IsConnected();
        if (connected && !clock_on) {
            home_clock_start();
            clock_on = true;
        } else if (!connected && clock_on) {
            home_clock_stop();
            clock_on = false;
        }

        // 资料(含头像)变化:先在锁外做文件 IO,再持锁更新 UI。
        const uint32_t profile_ver = home_profile_version();
        if (profile_ver != last_profile_ver) {
            last_profile_ver = profile_ver;
            char previous_lat[HOME_COORD_MAX], previous_lon[HOME_COORD_MAX];
            std::snprintf(previous_lat, sizeof(previous_lat), "%s", prof.lat);
            std::snprintf(previous_lon, sizeof(previous_lon), "%s", prof.lon);
            home_profile_get(&prof);
            if (std::strcmp(previous_lat, prof.lat) || std::strcmp(previous_lon, prof.lon))
                s_force_weather.store(true);
            const bool has = home_profile_has_avatar();
            esp_err_t err = ESP_FAIL;
            if (has) err = read_portrait(true);
            if (bsp_lvgl_lock(1000)) {
                if (s_ui_alive.load()) {
                    apply_profile(&prof);
                    apply_avatar(err == ESP_OK);
                }
                bsp_lvgl_unlock();
            }
        }

        // 首次进入延迟 15s;跨页面复用 30 分钟样本,手动刷新或换城市立即请求。
        home_profile_get(&prof);
        const unsigned requests = connected ? s_weather_schedule.due(now_ms, start_ms, s_force_weather.load(), prof) : 0;
        if (requests) {
            s_force_weather.store(false);
            s_weather_schedule.begin(requests, now_ms, prof);
            if (requests & WeatherSchedule::Weather) {
                const auto result = home_weather_refresh(prof.lat, prof.lon); // 阻塞数秒,不持锁
                if (s_home_active.load()) s_weather_schedule.finish(WeatherSchedule::Weather, esp_timer_get_time() / 1000, result);
            }
            if (s_home_active.load() && (requests & WeatherSchedule::Air)) {
                const auto result = home_air_quality_refresh(prof.lat, prof.lon);
                if (s_home_active.load()) s_weather_schedule.finish(WeatherSchedule::Air, esp_timer_get_time() / 1000, result);
            }
            if (!s_home_active.load()) break;
        }

        // 上传模式期间恢复即时下行(手机/设备往返都要快),结束时回到待机省电档。
        // 用电平变化检测,按键开启与 10 分钟自动超时两条路径都覆盖。
        const bool upload_active = home_upload_active();
        if (upload_active != upload_ps_high) {
            upload_ps_high = upload_active;
            WifiManager::GetInstance().SetPowerSaveLevel(
                upload_active ? WifiPowerSaveLevel::BALANCED : WifiPowerSaveLevel::LOW_POWER);
        }

        // 上传模式 10 分钟自动关闭,避免 httpd 长期占着内存。
        if (home_upload_active()) {
            if (upload_started_ms == 0) {
                upload_started_ms = now_ms;
            } else if (now_ms - upload_started_ms >= kUploadTimeoutMs) {
                home_upload_stop();
                upload_started_ms = 0;
            }
        } else {
            upload_started_ms = 0;
        }

        // 1s 周期拆成 10×100ms:stop() 后 ≤100ms 内退出任务,应用切换不再
        // 等满一秒 —— 切入小智/收音机时的过渡黑屏时间显著缩短。
        for (int i = 0; i < 10 && s_home_active.load(); ++i) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    s_task_handle = nullptr;
    s_worker_running.store(false);
    vTaskDelete(NULL);
}

void home_init() {
    home_profile_init();
    home_weather_init();
    // 只有 voice 应用调过 time_sync_init();首页自己调一次保证时区已载入(幂等 NVS 读)。
    time_sync_init();

    // LVGL 9 的 header 是位域结构,逐字段赋值比指定初始化器更稳(避免位域初始化顺序问题)。
    // Indexed colors preserve four foreground tones independently of transparency.
    s_portrait_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_portrait_dsc.header.cf = LV_COLOR_FORMAT_I4;
    s_portrait_dsc.header.flags = 0;
    s_portrait_dsc.header.w = HOME_PORTRAIT_W;
    s_portrait_dsc.header.h = HOME_PORTRAIT_H;
    s_portrait_dsc.header.stride = HOME_PORTRAIT_W / 2;
    s_portrait_dsc.data_size = HOME_PORTRAIT_I4_BYTES;
    s_portrait_dsc.data = s_portrait_pixels;

    ESP_LOGI(kTag, "Home app subsystems ready");
}

void home_start() {
    ESP_LOGI(kTag, "Starting Home app");
    home_weather_allow_requests();
    s_home_active.store(true);
    s_ui_alive.store(true);
    s_share_visible = false;

    // 首屏资料与人像在持锁前读完:SPIFFS 首次挂载可达几十毫秒,不应阻塞 LVGL 任务。
    // Allocate expanded pixels only when an uploaded portrait actually exists.

    home_profile_t prof;
    home_profile_get(&prof);
    const bool has_avatar = home_profile_has_avatar();
    const esp_err_t avatar_err =
        read_portrait(has_avatar);

    bsp_lvgl_lock(-1);
    if (!s_screen) create_ui();
    apply_profile(&prof);
    apply_avatar(avatar_err == ESP_OK);
    update_battery_label();
    lv_scr_load(s_screen);
    bsp_lvgl_unlock();

    // 栈给 8192 而不是文档里的 4096:本任务内会跑 HTTPS(mbedTLS 握手调用链很深),
    // 4KB 栈在证书校验阶段有溢出风险;另外天气图标走 LVGL TJPEG decoder_info
    // (文件源),它把 4KB 的 TJPGD_WORKBUFF_SIZE 放在栈上(lv_tjpgd.c:110),
    // 6144 在换工具链后实测爆栈(Stack protection fault,2026-09-13)。
    s_worker_running.store(true);
    BaseType_t created = xTaskCreate(home_task, "home_app", 8192, NULL, 4, &s_task_handle);
    if (created != pdPASS) {
        s_worker_running.store(false);
        s_task_handle = nullptr;
        ESP_LOGE(kTag, "Failed to create home task; free heap: %u KB",
                 (unsigned)(esp_get_free_heap_size() / 1024));
        return;
    }
    ESP_LOGI(kTag, "Home app started; free heap: %u KB",
             (unsigned)(esp_get_free_heap_size() / 1024));
}

bool home_prepare_stop() {
    s_home_active.store(false);
    s_ui_alive.store(false);
    home_weather_cancel_requests();
    return !s_worker_running.load();
}

void home_stop() {
    ESP_LOGI(kTag, "Stopping Home app");
    s_home_active.store(false);
    home_weather_cancel_requests();
    // 先撤掉 UI 开关:home_task 持锁刷新前会看它,避免"删屏后任务还在写标签"。
    s_ui_alive.store(false);

    // Registry calls this only after prepare_stop reports completion. Never
    // delete a task inside DNS/TLS: it may still own heap or socket locks.
    if (!home_prepare_stop()) return;

    home_upload_stop();
    home_clock_stop();

    bsp_lvgl_lock(-1);
    if (s_screen) {
        lv_obj_delete(s_screen);
    }
    s_screen = nullptr;
    s_avatar_img = nullptr;
    s_portrait_ph = nullptr;
    for (lv_obj_t *&bar : s_wifi_bars) bar = nullptr;
    s_lbl_clock = nullptr;
    s_batt = {nullptr, nullptr};
    s_lbl_name = nullptr;
    s_lbl_sig = nullptr;
    s_lbl_upload = nullptr;
    if (s_share_visible) {
        lv_display_t *disp = lv_display_get_default();
        if (disp) lv_display_enable_invalidation(disp, true);
        s_share_visible = false;
    }
    lv_image_cache_drop(&s_portrait_dsc);
    bsp_lvgl_unlock();

    // 屏已删、任务已退,人像缓冲可以还给堆了 —— 其它应用(小智)正等着
    // 这块连续内存开 Opus 编码器。dsc.data 一并清掉,避免悬挂引用。
    if (s_portrait_pixels != nullptr) {
        free(s_portrait_pixels);
        s_portrait_pixels = nullptr;
        s_portrait_dsc.data = nullptr;
    }

    free(s_portrait_packed);
    s_portrait_packed = nullptr;
    ESP_LOGI(kTag, "Home app stopped. Free heap: %u KB",
             (unsigned)(esp_get_free_heap_size() / 1024));
}

void home_on_key(uint8_t btn, app_btn_event_t event, uint16_t mv) {
    (void)mv;

    // 手机设置与首次开机共用同一个热点入口。
    if (btn == BSP_BTN_DOWN && event == BTN_EVT_LONG) {
        app_registry_switch_to(APP_ID_WIFI);
        return;
    }

    if (event != BTN_EVT_CLICK) return;

    if (s_share_visible && (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN)) {
        home_share_gallery_t gallery;
        if (home_profile_gallery_get(&gallery) == ESP_OK && gallery.count > 1) {
            const uint8_t previous = s_share_index;
            s_share_index = (s_share_index + gallery.count + (btn == BSP_BTN_UP ? -1 : 1)) % gallery.count;
            if (!home_show_share_image()) {
                s_share_index = previous;
                if (!home_show_share_image()) toggle_share_view();
            }
        }
        return;
    }

    // OK 短按 = 打开应用板块。
    if (btn == BSP_BTN_OK) {
        if (s_share_visible) {
            toggle_share_view();
            return;
        }
        ESP_LOGI(kTag, "Opening app drawer from Home");
        app_registry_switch_to(APP_ID_LAUNCHER);
        return;
    }

    if (btn == BSP_BTN_UP) {
        toggle_share_view();
        return;
    } else if (btn == BSP_BTN_DOWN) {
        toggle_share_view();
    }
}

}  // namespace

const passport_app_t g_home_app = {
    .id = APP_ID_HOME,
    .name = "Home",
    .en_name = "HOME",
    .desc = "Clock · Weather · Card · Upload",
    .tag = "Direct",
    .theme_color = 0xDDB36C,
    .init = home_init,
    .start = home_start,
    .stop = home_stop,
    .on_key = home_on_key,
    .prepare_stop = home_prepare_stop,
};
