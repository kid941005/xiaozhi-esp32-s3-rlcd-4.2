// 天气详情页：完整天气维度（实时 / 体感 / 湿度 / 风 / 能见度 / 气压 / AQI / 三日预报 / 日出日落 / 预警）
// 布局在横屏 400x300 与竖屏 300x400 下共用同一套行列结构，列宽按屏宽计算。
#include "custom_lcd_display.h"

#include <cstdio>
#include <cstring>
#include <string>

#include <esp_log.h>

#include "managers/weather_manager.h"

LV_FONT_DECLARE(font_puhui_16_4);
LV_FONT_DECLARE(font_puhui_14_1);
LV_FONT_DECLARE(font_puhui_16_1);

#define TAG "WeatherDetail"

namespace {

constexpr int kPad = 8;   // 页面左右留白
constexpr int kGap = 6;   // 卡片间距

// 空字符串降级为占位符
const char* nz(const std::string& s, const char* fallback) {
    return s.empty() ? fallback : s.c_str();
}

// 白底黑框圆角卡片
lv_obj_t* CreateDetailCard(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* card = lv_obj_create(parent);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, w, h);
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_black(), 0);
    lv_obj_set_style_radius(card, 6, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_CLICKABLE);
    return card;
}

}  // namespace

void CustomLcdDisplay::SetupWeatherDetailUI() {
    const int W = UI_SCR_W;
    const int H = UI_SCR_H;

    detail_page_ = lv_obj_create(lv_screen_active());
    lv_obj_set_size(detail_page_, W, H);
    lv_obj_set_pos(detail_page_, 0, 0);
    lv_obj_set_style_bg_color(detail_page_, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(detail_page_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(detail_page_, 0, 0);
    lv_obj_set_style_pad_all(detail_page_, 0, 0);
    lv_obj_remove_flag(detail_page_, LV_OBJ_FLAG_SCROLLABLE);

    // ===== 标题行：「天气」+ 城市 + 更新时间 =====
    lv_obj_t* title = lv_label_create(detail_page_);
    lv_obj_set_style_text_font(title, &font_puhui_16_1, 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, kPad, 8);
    lv_label_set_text(title, "天气");

    d_city_label_ = lv_label_create(detail_page_);
    lv_obj_set_style_text_font(d_city_label_, &font_puhui_16_4, 0);
    lv_obj_set_style_text_color(d_city_label_, lv_color_black(), 0);
    lv_obj_align(d_city_label_, LV_ALIGN_TOP_LEFT, kPad + 40, 8);
    lv_label_set_text(d_city_label_, "--");

    d_update_label_ = lv_label_create(detail_page_);
    lv_obj_set_style_text_font(d_update_label_, &font_puhui_14_1, 0);
    lv_obj_set_style_text_color(d_update_label_, lv_color_black(), 0);
    lv_obj_align(d_update_label_, LV_ALIGN_TOP_RIGHT, -kPad, 10);
    lv_label_set_text(d_update_label_, "更新 --:--");

    // ===== 六个信息格：体感 / 湿度 / 风 / 能见度 / 气压 / 今日温差 =====
    const int cell_w = (W - kPad * 2 - kGap * 2) / 3;
    const int cell_h = 40;

    // ===== 竖向弹性布局 =====
    // 各区块高度固定，把屏高多出的部分平均分配到区块间距：
    //   400x300 时 extra=0，各块位置与旧版完全一致；
    //   竖屏 300x400 时把多出的高度均摊，消除「预报」与「日出」之间约 108px 的空隙。
    const int cells_y = 34;                     // 六个信息格起始 y（标题行下方）
    const int cells_h = cell_h * 2 + 4;         // 两行 + 行间距
    const int aqi_h = 30;
    const int fc_h = 64;
    const int sun_h = 24;
    const int warn_h = 32;
    const int bottom_margin = 12;
    const int base_gap[4] = {4, 4, 8, 4};       // cells→AQI / AQI→预报 / 预报→日出 / 日出→预警
    const int gap_sum = base_gap[0] + base_gap[1] + base_gap[2] + base_gap[3];
    int extra = H - (cells_y + cells_h + aqi_h + fc_h + sun_h + warn_h + gap_sum + bottom_margin);
    if (extra < 0) extra = 0;
    int gap[4];
    for (int i = 0; i < 4; i++) {
        gap[i] = base_gap[i] + extra / 4 + (i < extra % 4 ? 1 : 0);
    }
    const int aqi_y = cells_y + cells_h + gap[0];
    const int fc_y = aqi_y + aqi_h + gap[1];
    const int sun_y = fc_y + fc_h + gap[2];
    const int warn_y = sun_y + sun_h + gap[3];

    static const char* kCellTitles[6] = {"体感", "湿度", "风", "能见度", "气压", "今日"};
    for (int i = 0; i < 6; i++) {
        const int col = i % 3;
        const int row = i / 3;
        const int x = kPad + col * (cell_w + kGap);
        const int y = cells_y + row * (cell_h + 4);
        lv_obj_t* card = CreateDetailCard(detail_page_, x, y, cell_w, cell_h);

        lv_obj_t* name = lv_label_create(card);
        lv_obj_set_style_text_font(name, &font_puhui_14_1, 0);
        lv_obj_set_style_text_color(name, lv_color_black(), 0);
        lv_obj_align(name, LV_ALIGN_TOP_LEFT, 6, 2);
        lv_label_set_text(name, kCellTitles[i]);

        d_cell_value_label_[i] = lv_label_create(card);
        lv_obj_set_style_text_font(d_cell_value_label_[i], &font_puhui_16_4, 0);
        lv_obj_set_style_text_color(d_cell_value_label_[i], lv_color_black(), 0);
        lv_obj_align(d_cell_value_label_[i], LV_ALIGN_TOP_LEFT, 6, 19);
        lv_label_set_text(d_cell_value_label_[i], "--");
    }

    // ===== AQI 条 =====
    lv_obj_t* aqi_card = CreateDetailCard(detail_page_, kPad, aqi_y, W - kPad * 2, aqi_h);
    d_aqi_label_ = lv_label_create(aqi_card);
    lv_obj_set_style_text_font(d_aqi_label_, &font_puhui_16_4, 0);
    lv_obj_set_style_text_color(d_aqi_label_, lv_color_black(), 0);
    lv_obj_align(d_aqi_label_, LV_ALIGN_LEFT_MID, 8, 0);
    lv_label_set_text(d_aqi_label_, "AQI --");

    // ===== 三日预报：今天 / 明天 / 后天 =====
    const int fc_w = (W - kPad * 2 - kGap * 2) / 3;
    for (int i = 0; i < 3; i++) {
        lv_obj_t* card = CreateDetailCard(detail_page_, kPad + i * (fc_w + kGap), fc_y, fc_w, fc_h);
        d_forecast_label_[i] = lv_label_create(card);
        lv_obj_set_style_text_font(d_forecast_label_[i], &font_puhui_16_4, 0);
        lv_obj_set_style_text_color(d_forecast_label_[i], lv_color_black(), 0);
        lv_obj_set_style_text_align(d_forecast_label_[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_line_space(d_forecast_label_[i], 2, 0);
        lv_obj_set_width(d_forecast_label_[i], fc_w - 8);
        lv_label_set_long_mode(d_forecast_label_[i], LV_LABEL_LONG_WRAP);
        lv_obj_center(d_forecast_label_[i]);
        lv_label_set_text(d_forecast_label_[i], "--\n--\n--~--°C");
    }

    // ===== 日出日落（贴近底部，随屏高浮动）=====
    lv_obj_t* sun_card = CreateDetailCard(detail_page_, kPad, sun_y, W - kPad * 2, sun_h);
    d_sun_label_ = lv_label_create(sun_card);
    lv_obj_set_style_text_font(d_sun_label_, &font_puhui_16_4, 0);
    lv_obj_set_style_text_color(d_sun_label_, lv_color_black(), 0);
    lv_obj_center(d_sun_label_);
    lv_label_set_text(d_sun_label_, "日出 --:--   日落 --:--");

    // ===== 预警条（无预警显示占位文案，保持布局稳定）=====
    lv_obj_t* warn_card = CreateDetailCard(detail_page_, kPad, warn_y, W - kPad * 2, warn_h);
    d_warn_label_ = lv_label_create(warn_card);
    lv_obj_set_style_text_font(d_warn_label_, &font_puhui_16_4, 0);
    lv_obj_set_style_text_color(d_warn_label_, lv_color_black(), 0);
    lv_obj_center(d_warn_label_);
    lv_label_set_text(d_warn_label_, "暂无气象预警");

    ESP_LOGI(TAG, "天气详情页创建完成（%dx%d，列宽 %d，预警条 y=%d）", W, H, cell_w, warn_y);
}

void CustomLcdDisplay::RefreshWeatherDetail() {
    if (detail_page_ == nullptr || d_city_label_ == nullptr) {
        return;
    }
    // WeatherData 内含 40 多个 std::string，放栈上会超过 -Werror=frame-larger-than=1536；
    // 本函数只在显示锁/UI 上下文串行调用，用 static 复用同一块存储。
    static WeatherData wd;
    wd = WeatherManager::getInstance().getLatestData();

    // 数据变更检测（用 char 缓冲拼指纹，避免大量 std::string 临时对象撑爆栈）
    static char stamp[512];
    static char last_stamp[512] = {0};
    const int stamp_n = snprintf(stamp, sizeof(stamp),
             "%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s",
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
             wd.daily_count > 2 ? nz(wd.daily[2].text_day, "") : "");
    // 变更检测：stamp 被截断时，两组不同数据可能共享同一被截断前缀而被 strcmp 漏判。
    // 因此一旦 snprintf 达到缓冲上限（无法可靠比较），直接置脏强制刷新，并清空基线。
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

    static char buf[128];

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

    // 六个信息格
    const char* cells[6];
    static char cell_buf[6][32];
    if (wd.feels_like.empty()) {
        cells[0] = "--";
    } else {
        snprintf(cell_buf[0], sizeof(cell_buf[0]), "%s°C", wd.feels_like.c_str());
        cells[0] = cell_buf[0];
    }
    if (wd.humidity.empty()) {
        cells[1] = "--";
    } else {
        snprintf(cell_buf[1], sizeof(cell_buf[1]), "%s%%", wd.humidity.c_str());
        cells[1] = cell_buf[1];
    }
    if (wd.wind_dir.empty() && wd.wind_scale.empty()) {
        cells[2] = "--";
    } else {
        snprintf(cell_buf[2], sizeof(cell_buf[2]), "%s%s%s%s", wd.wind_dir.c_str(),
                 wd.wind_scale.empty() ? "" : " ", wd.wind_scale.empty() ? "" : wd.wind_scale.c_str(),
                 wd.wind_scale.empty() ? "" : "级");
        cells[2] = cell_buf[2];
    }
    if (wd.vis.empty()) {
        cells[3] = "--";
    } else {
        snprintf(cell_buf[3], sizeof(cell_buf[3]), "%skm", wd.vis.c_str());
        cells[3] = cell_buf[3];
    }
    if (wd.pressure.empty()) {
        cells[4] = "--";
    } else {
        snprintf(cell_buf[4], sizeof(cell_buf[4]), "%shPa", wd.pressure.c_str());
        cells[4] = cell_buf[4];
    }
    if (wd.daily_count > 0) {
        snprintf(cell_buf[5], sizeof(cell_buf[5]), "%s~%s°C",
                 nz(wd.daily[0].temp_min, "--"), nz(wd.daily[0].temp_max, "--"));
        cells[5] = cell_buf[5];
    } else {
        cells[5] = "--";
    }
    for (int i = 0; i < 6; i++) {
        if (d_cell_value_label_[i] != nullptr) {
            lv_label_set_text(d_cell_value_label_[i], cells[i]);
        }
    }

    // AQI 条：AQI 值 + 类别 + PM2.5
    snprintf(buf, sizeof(buf), "AQI %s  %s   PM2.5 %s", nz(wd.aqi, "--"),
             nz(wd.aqi_category, ""), nz(wd.pm25, "--"));
    lv_label_set_text(d_aqi_label_, buf);

    // 三日预报
    static const char* kDayNames[3] = {"今天", "明天", "后天"};
    for (int i = 0; i < 3; i++) {
        if (i >= wd.daily_count) {
            snprintf(buf, sizeof(buf), "%s\n--\n--~--°C", kDayNames[i]);
        } else {
            snprintf(buf, sizeof(buf), "%s\n%s\n%s~%s°C", kDayNames[i],
                     nz(wd.daily[i].text_day, "--"), nz(wd.daily[i].temp_min, "--"),
                     nz(wd.daily[i].temp_max, "--"));
        }
        lv_label_set_text(d_forecast_label_[i], buf);
    }

    // 日出日落
    snprintf(buf, sizeof(buf), "日出 %s   日落 %s", nz(wd.sunrise, "--:--"),
             nz(wd.sunset, "--:--"));
    lv_label_set_text(d_sun_label_, buf);

    // 预警
    if (wd.warning.empty()) {
        lv_label_set_text(d_warn_label_, "暂无气象预警");
    } else {
        snprintf(buf, sizeof(buf), "预警  %s", wd.warning.c_str());
        lv_label_set_text(d_warn_label_, buf);
    }
}
