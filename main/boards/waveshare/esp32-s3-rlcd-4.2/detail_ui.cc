// 天气详情页：黑底 + 图标化的分层信息
//   标题行：定位图标 + 城市 + 更新时间
//   Hero  ：大温度 + 天气徽标 + 今日温差（主信息）
//           右侧体感 / 湿度 / 风（次信息，带图标与分隔线）
//   指标条：AQI（含 PM2.5）/ 能见度 / 气压（带图标）
//   预报  ：今天 / 明天 / 后天，最高温大字、最低温小字
//   底栏  ：日出日落 + 预警（有预警时黑底白字反色）
// 横屏 400x300 与竖屏 300x400 共用结构，竖屏把多余高度均摊到区块间距。
#include "custom_lcd_display.h"

#include <cstdio>
#include <cstring>
#include <string>

#include <esp_log.h>

#include "managers/weather_manager.h"
#include "material_symbols.h"
#include "ui_assets.h"

static const char *TAG = "WeatherDetail";

namespace {

constexpr int kPad = 8;    // 页面左右留白
constexpr int kGap = 6;    // 卡片间距
constexpr int kTitleY = 6;
constexpr int kTitleH = 18;
constexpr int kHeroY = kTitleY + kTitleH + kGap;   // 30
constexpr int kHeroLeftW = UI_PORTRAIT ? 160 : 206;
constexpr int kHeroH = UI_PORTRAIT ? 112 : 94;
constexpr int kMetricH = 44;
constexpr int kFcH = 66;
constexpr int kFooterH = 32;

// 空字符串降级为占位符
const char* nz(const std::string& s, const char* fallback) {
    return s.empty() ? fallback : s.c_str();
}

// UTF-8 字符数（中文按 3 字节算，用于决定徽标宽度）
int Utf8Chars(const std::string& s) {
    int n = 0;
    for (unsigned char c : s) {
        if ((c & 0xC0) != 0x80) {
            ++n;
        }
    }
    return n;
}

// 白底黑框圆角卡片
lv_obj_t* CreateDetailCard(lv_obj_t* parent, int x, int y, int w, int h, int border = 2) {
    lv_obj_t* card = lv_obj_create(parent);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, w, h);
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, border, 0);
    lv_obj_set_style_border_color(card, lv_color_black(), 0);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_CLICKABLE);
    return card;
}

// 在 parent 的 (x, y) 放一个文本标签
lv_obj_t* MakeLabel(lv_obj_t* parent, const lv_font_t* font, lv_color_t color,
                    int x, int y, const char* text) {
    lv_obj_t* label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_pos(label, x, y);
    lv_label_set_text(label, text);
    return label;
}

// 图标标签（Material Symbols，16px 4bpp）
lv_obj_t* MakeIcon(lv_obj_t* parent, const char* symbol, lv_color_t color,
                   int x, int y) {
    return MakeLabel(parent, &font_material_symbols_16_4, color, x, y, symbol);
}

// 1px 分隔线
void MakeDivider(lv_obj_t* parent, int x, int y, int w) {
    lv_obj_t* line = lv_obj_create(parent);
    lv_obj_set_pos(line, x, y);
    lv_obj_set_size(line, w, 1);
    lv_obj_set_style_bg_color(line, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_set_style_radius(line, 0, 0);
    lv_obj_set_style_pad_all(line, 0, 0);
    lv_obj_remove_flag(line, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(line, LV_OBJ_FLAG_CLICKABLE);
}

}  // namespace

void CustomLcdDisplay::SetupWeatherDetailUI() {
    const int W = UI_SCR_W;
    const int H = UI_SCR_H;
    const int content_w = W - kPad * 2;

    // 黑底：与天气主页保持一致的默认底色
    detail_page_ = lv_obj_create(lv_screen_active());
    lv_obj_set_size(detail_page_, W, H);
    lv_obj_set_pos(detail_page_, 0, 0);
    lv_obj_set_style_bg_color(detail_page_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(detail_page_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(detail_page_, 0, 0);
    lv_obj_set_style_pad_all(detail_page_, 0, 0);
    lv_obj_set_style_radius(detail_page_, 0, 0);
    lv_obj_remove_flag(detail_page_, LV_OBJ_FLAG_SCROLLABLE);

    // ===== 标题行：定位图标 + 「天气」+ 城市 + 更新时间 =====
    MakeIcon(detail_page_, MATERIAL_SYMBOLS_LOCATION_ON, lv_color_white(), kPad, kTitleY + 1);
    MakeLabel(detail_page_, &font_puhui_16_1, lv_color_white(), kPad + 20, kTitleY, "天气");

    d_city_label_ = lv_label_create(detail_page_);
    lv_obj_set_style_text_font(d_city_label_, &font_puhui_16_4, 0);
    lv_obj_set_style_text_color(d_city_label_, lv_color_white(), 0);
    lv_obj_set_pos(d_city_label_, kPad + 56, kTitleY - 1);
    lv_obj_set_width(d_city_label_, 130);
    lv_label_set_long_mode(d_city_label_, LV_LABEL_LONG_CLIP);
    lv_label_set_text(d_city_label_, "--");

    d_update_label_ = lv_label_create(detail_page_);
    lv_obj_set_style_text_font(d_update_label_, &font_puhui_14_1, 0);
    lv_obj_set_style_text_color(d_update_label_, lv_color_white(), 0);
    lv_obj_align(d_update_label_, LV_ALIGN_TOP_RIGHT, -kPad, kTitleY);
    lv_label_set_text(d_update_label_, "更新 --:--");
    lv_obj_t* update_icon = MakeIcon(detail_page_, MATERIAL_SYMBOLS_SCHEDULE,
                                     lv_color_white(), 0, 0);
    lv_obj_align_to(update_icon, d_update_label_, LV_ALIGN_OUT_LEFT_MID, -12, 0);

    // ===== 区块位置：横屏固定；竖屏把多余高度均摊到 4 个间距 =====
    int metrics_y;
    int fc_y;
    int sun_y;
    int warn_y;
#if UI_PORTRAIT
    const int fixed = kHeroY + kHeroH + kGap + kMetricH + kGap + kFcH + kGap +
                      (kFooterH * 2 + kGap) + kPad;
    int extra = H - fixed;
    if (extra < 0) extra = 0;
    const int add = extra / 4;
    metrics_y = kHeroY + kHeroH + kGap + add;
    fc_y = metrics_y + kMetricH + kGap + add;
    sun_y = fc_y + kFcH + kGap + add;
    warn_y = sun_y + kFooterH + kGap;
#else
    metrics_y = kHeroY + kHeroH + kGap;       // 130
    fc_y = metrics_y + kMetricH + kGap;       // 180
    sun_y = H - kPad - kFooterH;              // 260
    warn_y = sun_y;
#endif

    // ===== Hero 左：天气徽标 + 大温度 + 今日温差 =====
    lv_obj_t* hero_left = CreateDetailCard(detail_page_, kPad, kHeroY, kHeroLeftW, kHeroH);

    d_hero_cond_badge_ = lv_obj_create(hero_left);
    lv_obj_set_pos(d_hero_cond_badge_, 8, 7);
    lv_obj_set_size(d_hero_cond_badge_, 44, 28);
    lv_obj_set_style_bg_color(d_hero_cond_badge_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(d_hero_cond_badge_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(d_hero_cond_badge_, 0, 0);
    lv_obj_set_style_radius(d_hero_cond_badge_, 6, 0);
    lv_obj_set_style_pad_all(d_hero_cond_badge_, 0, 0);
    lv_obj_remove_flag(d_hero_cond_badge_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(d_hero_cond_badge_, LV_OBJ_FLAG_CLICKABLE);

    d_hero_cond_label_ = lv_label_create(d_hero_cond_badge_);
    lv_obj_set_style_text_font(d_hero_cond_label_, &font_puhui_14_1, 0);
    lv_obj_set_style_text_color(d_hero_cond_label_, lv_color_white(), 0);
    lv_obj_center(d_hero_cond_label_);
    lv_label_set_text(d_hero_cond_label_, "--");

    d_hero_temp_label_ = lv_label_create(hero_left);
    lv_obj_set_style_text_font(d_hero_temp_label_, &alibaba_puhui_48, 0);
    lv_obj_set_style_text_color(d_hero_temp_label_, lv_color_black(), 0);
    lv_obj_set_pos(d_hero_temp_label_, 60, 1);
    lv_obj_set_width(d_hero_temp_label_, kHeroLeftW - 64);
    lv_label_set_long_mode(d_hero_temp_label_, LV_LABEL_LONG_CLIP);
    lv_label_set_text(d_hero_temp_label_, "--");

    d_hero_range_label_ = MakeLabel(hero_left, &font_puhui_14_1, lv_color_black(),
                                    10, kHeroH - 24, "今日 --~--°C");
    lv_obj_set_width(d_hero_range_label_, kHeroLeftW - 20);
    lv_label_set_long_mode(d_hero_range_label_, LV_LABEL_LONG_CLIP);

    // ===== Hero 右：体感 / 湿度 / 风（图标 + 标签 + 值，行间分隔线）=====
    const int hero_right_w = content_w - kHeroLeftW - kGap;
    lv_obj_t* hero_right = CreateDetailCard(detail_page_, kPad + kHeroLeftW + kGap,
                                            kHeroY, hero_right_w, kHeroH, 1);
    static const char* kExtraNames[3] = {"体感", "湿度", "风"};
    static const char* kExtraIcons[3] = {
        MATERIAL_SYMBOLS_DEVICE_THERMOSTAT,
        MATERIAL_SYMBOLS_CLOUD_DOWNLOAD,
        MATERIAL_SYMBOLS_NAVIGATION,
    };
    const int row_h = (kHeroH - 8) / 3;
    for (int i = 0; i < 3; ++i) {
        const int ry = 4 + i * row_h;
        MakeIcon(hero_right, kExtraIcons[i], lv_color_black(), 8, ry);
        MakeLabel(hero_right, &font_puhui_14_1, lv_color_black(), 28, ry + 1,
                  kExtraNames[i]);

        d_hero_extra_value_[i] = lv_label_create(hero_right);
        // 竖屏右卡只有 118 宽，用 14px 全字符集字体并收窄左侧标签占位，避免风值被裁
        lv_obj_set_style_text_font(d_hero_extra_value_[i],
                                   UI_PORTRAIT ? &font_puhui_14_1 : &font_puhui_16_4, 0);
        lv_obj_set_style_text_color(d_hero_extra_value_[i], lv_color_black(), 0);
        lv_obj_set_width(d_hero_extra_value_[i], hero_right_w - (UI_PORTRAIT ? 36 : 62));
        lv_obj_set_style_text_align(d_hero_extra_value_[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_long_mode(d_hero_extra_value_[i], LV_LABEL_LONG_CLIP);
        lv_obj_align(d_hero_extra_value_[i], LV_ALIGN_TOP_RIGHT, -8, ry - 1);
        lv_label_set_text(d_hero_extra_value_[i], "--");

        if (i < 2) {
            MakeDivider(hero_right, 8, ry + row_h - 2, hero_right_w - 16);
        }
    }

    // ===== 指标条：AQI（含 PM2.5）/ 能见度 / 气压（标题 + 图标 + 值）=====
    const int metric_w = (content_w - kGap * 2) / 3;
    for (int i = 0; i < 3; ++i) {
        lv_obj_t* card = CreateDetailCard(detail_page_, kPad + i * (metric_w + kGap),
                                          metrics_y, metric_w, kMetricH, 1);
        static const char* kMetricNames[3] = {"AQI", "能见度", "气压"};
        static const char* kMetricIcons[3] = {
            MATERIAL_SYMBOLS_FAVORITE,
            MATERIAL_SYMBOLS_EYEGLASSES,
            MATERIAL_SYMBOLS_PROGRESS_ACTIVITY,
        };
        MakeLabel(card, &font_puhui_14_1, lv_color_black(), 8, 4, kMetricNames[i]);
        // AQI 卡右上角已有 PM2.5 数值，再放图标会与文字重叠，故仅其余卡显示图标
        if (i != 0) {
            MakeIcon(card, kMetricIcons[i], lv_color_black(), metric_w - 24, 4);
        }

        if (i == 0) {
            d_metric_value_[0] = MakeLabel(card, &font_puhui_16_4, lv_color_black(),
                                           8, 21, "--");
            d_metric_sub_[0] = MakeLabel(card, &font_puhui_14_1, lv_color_black(),
                                         8, 3, "");
            lv_obj_align(d_metric_sub_[0], LV_ALIGN_TOP_RIGHT, -8, 5);
        } else {
            d_metric_value_[i] = MakeLabel(card, &font_puhui_16_4, lv_color_black(),
                                           8, 21, "--");
        }
        if (d_metric_value_[i] != nullptr) {
            lv_obj_set_width(d_metric_value_[i], metric_w - 16);
            lv_label_set_long_mode(d_metric_value_[i], LV_LABEL_LONG_CLIP);
        }
    }

    // ===== 三日预报：最高温大字、最低温小字 =====
    const int fc_w = (content_w - kGap * 2) / 3;
    static const char* kDayNames[3] = {"今天", "明天", "后天"};
    for (int i = 0; i < 3; ++i) {
        lv_obj_t* card = CreateDetailCard(detail_page_, kPad + i * (fc_w + kGap),
                                          fc_y, fc_w, kFcH, 1);
        d_fc_day_[i] = MakeLabel(card, &font_puhui_14_1, lv_color_black(), 0, 3,
                                 kDayNames[i]);
        lv_obj_align(d_fc_day_[i], LV_ALIGN_TOP_MID, 0, 3);

        d_fc_text_[i] = MakeLabel(card, &font_puhui_16_4, lv_color_black(), 0, 19, "--");
        lv_obj_align(d_fc_text_[i], LV_ALIGN_TOP_MID, 0, 19);

        d_fc_high_[i] = lv_label_create(card);
        lv_obj_set_style_text_font(d_fc_high_[i], &alibaba_puhui_24, 0);
        lv_obj_set_style_text_color(d_fc_high_[i], lv_color_black(), 0);
        lv_obj_align(d_fc_high_[i], LV_ALIGN_BOTTOM_LEFT, UI_PORTRAIT ? 6 : 10, -1);
        lv_label_set_text(d_fc_high_[i], "--°");

        d_fc_low_[i] = lv_label_create(card);
        lv_obj_set_style_text_font(d_fc_low_[i], &font_puhui_14_1, 0);
        lv_obj_set_style_text_color(d_fc_low_[i], lv_color_black(), 0);
        lv_obj_align(d_fc_low_[i], LV_ALIGN_BOTTOM_RIGHT, UI_PORTRAIT ? -6 : -8, -3);
        lv_label_set_text(d_fc_low_[i], "/--°");
    }

    // ===== 底栏：日出日落 + 预警 =====
#if UI_PORTRAIT
    const int sun_w = content_w;
#else
    const int sun_w = 190;
#endif
    lv_obj_t* sun_card = CreateDetailCard(detail_page_, kPad, sun_y, sun_w, kFooterH, 1);
    MakeIcon(sun_card, MATERIAL_SYMBOLS_BRIGHTNESS_6, lv_color_black(), 8, 8);
    d_sun_label_ = MakeLabel(sun_card, &font_puhui_14_1, lv_color_black(), 28, 8,
                             "日出 --:--   日落 --:--");
    lv_obj_set_width(d_sun_label_, sun_w - 36);
    lv_label_set_long_mode(d_sun_label_, LV_LABEL_LONG_CLIP);

#if UI_PORTRAIT
    const int warn_x = kPad;
    const int warn_w = content_w;
#else
    const int warn_x = kPad + sun_w + kGap;
    const int warn_w = W - kPad - warn_x;
#endif
    d_warn_card_ = CreateDetailCard(detail_page_, warn_x, warn_y, warn_w, kFooterH, 1);
    d_warn_icon_ = MakeIcon(d_warn_card_, MATERIAL_SYMBOLS_WARNING, lv_color_black(), 8, 8);
    d_warn_label_ = lv_label_create(d_warn_card_);
    lv_obj_set_style_text_font(d_warn_label_, &font_puhui_16_4, 0);
    lv_obj_set_style_text_color(d_warn_label_, lv_color_black(), 0);
    lv_obj_set_pos(d_warn_label_, 28, 7);
    lv_obj_set_width(d_warn_label_, warn_w - 36);
    lv_obj_set_style_text_align(d_warn_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(d_warn_label_, LV_LABEL_LONG_CLIP);
    lv_label_set_text(d_warn_label_, "暂无气象预警");

    ESP_LOGI(TAG, "天气详情页创建完成（%dx%d，黑底，hero %dx%d，指标 y=%d，预报 y=%d，预警 y=%d）",
             W, H, kHeroLeftW, kHeroH, metrics_y, fc_y, warn_y);
}

void CustomLcdDisplay::RefreshWeatherDetail() {
    if (detail_page_ == nullptr || d_city_label_ == nullptr) {
        return;
    }
    // WeatherData 内含 40 多个 std::string，放栈上会超过 -Werror=frame-larger-than=1536；
    // 本函数只在显示锁/UI 上下文串行调用，用 static 复用同一块存储。
    static WeatherData wd;
    wd = WeatherManager::getInstance().getLatestData();

    // 数据变更检测：包含页面实际展示的所有字段（含三日预报高低温）
    static char stamp[512];
    static char last_stamp[512] = {0};
    const int stamp_n = snprintf(stamp, sizeof(stamp),
             "%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s",
             nz(wd.city, ""), nz(wd.temp, ""), nz(wd.text, ""), nz(wd.update_time, ""),
             nz(wd.feels_like, ""), nz(wd.humidity, ""), nz(wd.wind_dir, ""),
             nz(wd.wind_scale, ""), nz(wd.vis, ""), nz(wd.pressure, ""), nz(wd.aqi, ""),
             nz(wd.aqi_category, ""), nz(wd.pm25, ""), nz(wd.sunrise, ""), nz(wd.sunset, ""),
             nz(wd.warning, ""),
             wd.daily_count > 0 ? nz(wd.daily[0].text_day, "") : "",
             wd.daily_count > 0 ? nz(wd.daily[0].temp_min, "") : "",
             wd.daily_count > 0 ? nz(wd.daily[0].temp_max, "") : "",
             wd.daily_count > 1 ? nz(wd.daily[1].text_day, "") : "",
             wd.daily_count > 1 ? nz(wd.daily[1].temp_min, "") : "",
             wd.daily_count > 1 ? nz(wd.daily[1].temp_max, "") : "",
             wd.daily_count > 2 ? nz(wd.daily[2].text_day, "") : "",
             wd.daily_count > 2 ? nz(wd.daily[2].temp_min, "") : "",
             wd.daily_count > 2 ? nz(wd.daily[2].temp_max, "") : "",
             wd.daily_count > 0 ? nz(wd.daily[0].wind_dir, "") : "");
    // stamp 被截断时两组不同数据可能共享同一前缀而被漏判，直接置脏强制刷新
    bool stamp_dirty;
    if (stamp_n < 0 || stamp_n >= (int)sizeof(stamp)) {
        stamp_dirty = true;
        last_stamp[0] = '\0';
    } else {
        stamp_dirty = (strcmp(stamp, last_stamp) != 0);
        if (stamp_dirty) {
            memcpy(last_stamp, stamp, (size_t)stamp_n + 1);
        }
    }
    if (!stamp_dirty) {
        return;
    }

    static char buf[160];

    // 城市 + 更新时间（update_time 形如 2026-11-17T09:50+08:00，取 HH:MM）
    lv_label_set_text(d_city_label_, nz(wd.city, "--"));
    char hm[8] = "--:--";
    const char* tp = strchr(nz(wd.update_time, ""), 'T');
    if (tp != nullptr && strlen(tp) >= 6) {
        memcpy(hm, tp + 1, 5);
        hm[5] = '\0';
    }
    snprintf(buf, sizeof(buf), "更新 %s", hm);
    lv_label_set_text(d_update_label_, buf);

    // ===== Hero：天气徽标 + 大温度 + 今日温差 =====
    const std::string cond = wd.text.empty() ? "--" : wd.text;
    const int cond_chars = Utf8Chars(cond);
    const lv_font_t* cond_font = &font_puhui_14_1;
    int badge_w = 34;
    if (cond_chars == 2) badge_w = 44;
    else if (cond_chars == 3) badge_w = 58;
    else if (cond_chars >= 4) badge_w = 74;
    // 竖屏左卡只有 160 宽：徽标最多 58，给 48px 温度留出 80px
    const int badge_max = UI_PORTRAIT ? 58 : (kHeroLeftW - 84);
    if (badge_w > badge_max) badge_w = badge_max;

    lv_obj_set_size(d_hero_cond_badge_, badge_w, 28);
    lv_obj_set_style_text_font(d_hero_cond_label_, cond_font, 0);
    lv_label_set_text(d_hero_cond_label_, cond.c_str());
    lv_obj_center(d_hero_cond_label_);

    // 竖屏下 3 位数温度（如 -10）用 24px，避免被卡片裁掉；其余仍用 48px 大字
    const bool small_temp = UI_PORTRAIT && wd.temp.size() > 2;
    lv_obj_set_style_text_font(d_hero_temp_label_,
                               small_temp ? &alibaba_puhui_24 : &alibaba_puhui_48, 0);
    const int temp_x = 8 + badge_w + 6;
    lv_obj_set_pos(d_hero_temp_label_, temp_x, small_temp ? 20 : 1);
    lv_obj_set_width(d_hero_temp_label_, kHeroLeftW - temp_x - 4);
    if (wd.temp.empty()) {
        lv_label_set_text(d_hero_temp_label_, "--");
    } else {
        snprintf(buf, sizeof(buf), "%s°", wd.temp.c_str());
        lv_label_set_text(d_hero_temp_label_, buf);
    }

    if (wd.daily_count > 0) {
        snprintf(buf, sizeof(buf), "今日 %s~%s°C",
                 nz(wd.daily[0].temp_min, "--"), nz(wd.daily[0].temp_max, "--"));
    } else {
        snprintf(buf, sizeof(buf), "今日 --~--°C");
    }
    lv_label_set_text(d_hero_range_label_, buf);

    // 体感 / 湿度 / 风
    if (wd.feels_like.empty()) {
        snprintf(buf, sizeof(buf), "--");
    } else {
        snprintf(buf, sizeof(buf), "%s°C", wd.feels_like.c_str());
    }
    lv_label_set_text(d_hero_extra_value_[0], buf);

    if (wd.humidity.empty()) {
        snprintf(buf, sizeof(buf), "--");
    } else {
        snprintf(buf, sizeof(buf), "%s%%", wd.humidity.c_str());
    }
    lv_label_set_text(d_hero_extra_value_[1], buf);

    if (wd.wind_dir.empty() && wd.wind_scale.empty()) {
        snprintf(buf, sizeof(buf), "--");
    } else {
        snprintf(buf, sizeof(buf), "%s%s%s%s", wd.wind_dir.c_str(),
                 wd.wind_scale.empty() ? "" : " ",
                 wd.wind_scale.empty() ? "" : wd.wind_scale.c_str(),
                 wd.wind_scale.empty() ? "" : "级");
    }
    lv_label_set_text(d_hero_extra_value_[2], buf);

    // ===== 指标条 =====
    if (wd.aqi.empty()) {
        snprintf(buf, sizeof(buf), "--");
    } else if (wd.aqi_category.empty()) {
        snprintf(buf, sizeof(buf), "%s", wd.aqi.c_str());
    } else {
        snprintf(buf, sizeof(buf), "%s %s", wd.aqi.c_str(), wd.aqi_category.c_str());
    }
    lv_label_set_text(d_metric_value_[0], buf);
    snprintf(buf, sizeof(buf), "PM2.5 %s", nz(wd.pm25, "--"));
    lv_label_set_text(d_metric_sub_[0], buf);

    if (wd.vis.empty()) {
        snprintf(buf, sizeof(buf), "--");
    } else {
        snprintf(buf, sizeof(buf), "%skm", wd.vis.c_str());
    }
    lv_label_set_text(d_metric_value_[1], buf);

    if (wd.pressure.empty()) {
        snprintf(buf, sizeof(buf), "--");
    } else {
        snprintf(buf, sizeof(buf), "%shPa", wd.pressure.c_str());
    }
    lv_label_set_text(d_metric_value_[2], buf);

    // ===== 三日预报 =====
    static const char* kDayNames[3] = {"今天", "明天", "后天"};
    for (int i = 0; i < 3; ++i) {
        lv_label_set_text(d_fc_day_[i], kDayNames[i]);
        if (i >= wd.daily_count) {
            lv_label_set_text(d_fc_text_[i], "--");
            lv_label_set_text(d_fc_high_[i], "--°");
            lv_label_set_text(d_fc_low_[i], "/--°");
            continue;
        }
        lv_label_set_text(d_fc_text_[i], nz(wd.daily[i].text_day, "--"));
        snprintf(buf, sizeof(buf), "%s°", nz(wd.daily[i].temp_max, "--"));
        lv_label_set_text(d_fc_high_[i], buf);
        snprintf(buf, sizeof(buf), "/%s°", nz(wd.daily[i].temp_min, "--"));
        lv_label_set_text(d_fc_low_[i], buf);
    }

    // ===== 日出日落 =====
    snprintf(buf, sizeof(buf), "日出 %s  日落 %s", nz(wd.sunrise, "--:--"),
             nz(wd.sunset, "--:--"));
    lv_label_set_text(d_sun_label_, buf);

    // ===== 预警：有预警反色（黑底白字 + 白框）突出显示 =====
    if (wd.warning.empty()) {
        lv_obj_set_style_bg_color(d_warn_card_, lv_color_white(), 0);
        lv_obj_set_style_border_color(d_warn_card_, lv_color_black(), 0);
        lv_obj_set_style_text_color(d_warn_label_, lv_color_black(), 0);
        lv_obj_set_style_text_color(d_warn_icon_, lv_color_black(), 0);
        lv_label_set_text(d_warn_label_, "暂无气象预警");
    } else {
        lv_obj_set_style_bg_color(d_warn_card_, lv_color_black(), 0);
        lv_obj_set_style_border_color(d_warn_card_, lv_color_white(), 0);
        lv_obj_set_style_text_color(d_warn_label_, lv_color_white(), 0);
        lv_obj_set_style_text_color(d_warn_icon_, lv_color_white(), 0);
        snprintf(buf, sizeof(buf), "预警  %s", wd.warning.c_str());
        lv_label_set_text(d_warn_label_, buf);
    }
}
