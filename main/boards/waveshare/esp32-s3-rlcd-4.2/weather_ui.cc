// 天气站 UI 布局（复刻微雪 RLCD 4.2 成品界面）
//
// 横屏 400×300：
//   状态栏：左上「环境 32.1°C 39%」   右上白底胶囊（WiFi + 电池 + 电量%）
//   ┌───────────────┬───────────────┐
//   │ 时钟卡 189×138 │ 待办卡 189×256 │
//   │ 10月2日 星期五 │ 待办      ☑ ★ │
//   │     17:18      │ ★ 抢火车票     │
//   │ 十月初八 小雪  │ ★ 记得吃饭     │
//   │ 成都 晴 23°C   │               │
//   ├───────────────┤               │
//   │ AI 卡 189×112  │               │
//   └───────────────┴───────────────┘
// 竖屏 300×400：时钟卡 → 待办卡 → AI 卡，整宽纵向堆叠
//
// 控件归属（成员变量定义在 custom_lcd_display.h）：
//   day_label_     日期行「10月2日 星期五」
//   time_label_    大字时钟「17:18」
//   lunar_label_   农历/节气/节日「十月初八 小雪」
//   weather_label_ 天气行「成都 晴 23°C」
//   memo_rows_     待办列表容器（RefreshMemoDisplayInternal 动态生成行）
//   chat_card_ 等  AI 对话卡片

#include "custom_lcd_display.h"
#include "material_symbols.h"
#include <esp_log.h>

// 天气站专用字体（从 MyWeatherStation 移植，字符集有限但够天气站用）
LV_FONT_DECLARE(alibaba_puhui_16);
LV_FONT_DECLARE(alibaba_puhui_24);
LV_FONT_DECLARE(alibaba_puhui_48);
LV_FONT_DECLARE(alibaba_black_64);

// 小智自带字体（7415 个常用汉字，用于中文文案）
LV_FONT_DECLARE(font_puhui_16_4);  // 16px 标准字体
LV_FONT_DECLARE(font_puhui_14_1);  // 14px 小字体
LV_FONT_DECLARE(font_material_symbols_16_4);  // 16px 图标字体（★ ☑）

// 声明状态栏图标（从 MyWeatherStation 移植）
LV_IMAGE_DECLARE(ui_img_wifi);
LV_IMAGE_DECLARE(ui_img_wifi_low);
LV_IMAGE_DECLARE(ui_img_wifi_off);
LV_IMAGE_DECLARE(ui_img_battery_full);
LV_IMAGE_DECLARE(ui_img_battery_medium);
LV_IMAGE_DECLARE(ui_img_battery_low);
LV_IMAGE_DECLARE(ui_img_battery_charging);

static const char *TAG = "WeatherUI";

void CustomLcdDisplay::SetupWeatherUI() {
    DisplayLockGuard lock(this);

    lv_obj_t *root = lv_screen_active();
    lv_obj_set_style_bg_color(root, lv_color_black(), 0);
    weather_page_ = lv_obj_create(root);
    lv_obj_set_size(weather_page_, UI_SCR_W, UI_SCR_H);
    lv_obj_set_pos(weather_page_, 0, 0);
    lv_obj_set_style_bg_opa(weather_page_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(weather_page_, 0, 0);
    lv_obj_set_style_pad_all(weather_page_, 0, 0);
    lv_obj_set_style_radius(weather_page_, 0, 0);
    lv_obj_remove_flag(weather_page_, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *screen = weather_page_;

    const lv_font_t *font_small  = &alibaba_puhui_16;
    const lv_font_t *font_clock  = &alibaba_black_64;
    const lv_font_t *font_ai     = &font_puhui_16_4;   // 7415 常用汉字
    const lv_font_t *font_tiny   = &font_puhui_14_1;   // 14px
    const lv_font_t *font_icon   = &font_material_symbols_16_4;

    // ===== 状态栏（右上角白底胶囊：WiFi + 电池 + 电量）=====
    lv_obj_t *status_bar = lv_obj_create(screen);
    lv_obj_set_size(status_bar, 110, 24);
    lv_obj_set_style_bg_opa(status_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(status_bar, lv_color_white(), 0);
    lv_obj_set_style_border_width(status_bar, 0, 0);
    lv_obj_set_style_radius(status_bar, 14, 0);
    lv_obj_align(status_bar, LV_ALIGN_TOP_RIGHT, -10, 9);
    lv_obj_set_style_pad_all(status_bar, 0, 0);
    lv_obj_set_style_pad_left(status_bar, 8, 0);
    lv_obj_set_style_pad_right(status_bar, 8, 0);
    lv_obj_remove_flag(status_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(status_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_bar, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(status_bar, 5, 0);

    wifi_icon_img_ = lv_image_create(status_bar);
    lv_image_set_src(wifi_icon_img_, &ui_img_wifi_off);

    battery_icon_img_ = lv_image_create(status_bar);
    lv_image_set_src(battery_icon_img_, &ui_img_battery_full);

    battery_pct_label_ = lv_label_create(status_bar);
    lv_obj_set_style_text_font(battery_pct_label_, font_small, 0);
    lv_obj_set_style_text_color(battery_pct_label_, lv_color_black(), 0);
    lv_label_set_text(battery_pct_label_, "---%");

    // ===== 左上角「环境 温度 湿度」（白字，直接在黑底上）=====
    // 注意：必须用 font_puhui_16_4（7415 常用汉字），alibaba_puhui_16 字符集缺「环境」二字
    sensor_label_ = lv_label_create(screen);
    lv_obj_set_style_text_font(sensor_label_, font_ai, 0);
    lv_obj_set_style_text_color(sensor_label_, lv_color_white(), 0);
    lv_obj_align(sensor_label_, LV_ALIGN_TOP_LEFT, 10, 10);
    lv_label_set_text(sensor_label_, "环境 --.-°C --%");

    // 农历 / 节气 / 节日：放在顶栏右侧（胶囊左边），不占用时钟卡的四行结构
    lunar_label_ = lv_label_create(screen);
    lv_obj_set_style_text_font(lunar_label_, font_tiny, 0);
    lv_obj_set_style_text_color(lunar_label_, lv_color_white(), 0);
    lv_obj_align(lunar_label_, LV_ALIGN_TOP_RIGHT, -128, 12);
    lv_label_set_text(lunar_label_, "");

    // ===== 卡片几何 =====
    const int pad = 10;
    const int gap = 8;
    const int top = 41;
#if UI_PORTRAIT
    // 竖屏 300×400：时钟 → 待办 → AI，整宽纵向堆叠
    const int col_w   = UI_SCR_W - pad * 2;                  // 280
    const int clock_x = pad;
    const int clock_y = top;
    const int clock_w = col_w;
    const int clock_h = 140;
    const int todo_x  = pad;
    const int todo_y  = clock_y + clock_h + gap;             // 189
    const int todo_w  = col_w;
    const int todo_h  = 138;
    const int ai_x    = pad;
    const int ai_y    = todo_y + todo_h + gap;               // 335
    const int ai_w    = col_w;
    const int ai_h    = UI_SCR_H - ai_y - pad;               // 55
#else
    // 横屏 400×300：左栏（时钟 + AI）+ 右栏（整高待办），尺寸按微雪成品界面实测比例
    const int col_w   = 199;                                 // 左栏宽
    const int clock_x = pad;
    const int clock_y = top;
    const int clock_w = col_w;
    const int clock_h = 120;
    const int todo_x  = pad + col_w + gap;                   // 217
    const int todo_y  = top;
    const int todo_w  = UI_SCR_W - pad * 2 - gap - col_w;    // 173
    const int todo_h  = UI_SCR_H - top - 10;                 // 249
    const int ai_x    = pad;
    const int ai_y    = clock_y + clock_h + 18;              // 179
    const int ai_w    = col_w;
    const int ai_h    = 100;
#endif

    // ===== 时钟卡片：日期 / 大字时间 / 农历 / 天气 =====
    lv_obj_t *clock_card = lv_obj_create(screen);
    lv_obj_set_pos(clock_card, clock_x, clock_y);
    lv_obj_set_size(clock_card, clock_w, clock_h);
    lv_obj_set_style_border_width(clock_card, 2, 0);
    lv_obj_set_style_border_color(clock_card, lv_color_black(), 0);
    lv_obj_set_style_radius(clock_card, 15, 0);
    lv_obj_set_style_bg_color(clock_card, lv_color_white(), 0);
    lv_obj_set_style_pad_all(clock_card, 0, 0);
    lv_obj_remove_flag(clock_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_clip_corner(clock_card, true, 0);

    // 日期行：「10月2日 星期五」（黑字白底，卡片顶部）
    day_label_ = lv_label_create(clock_card);
    lv_obj_set_style_text_font(day_label_, font_ai, 0);
    lv_obj_set_style_text_color(day_label_, lv_color_black(), 0);
    lv_obj_align(day_label_, LV_ALIGN_TOP_MID, 0, 8);
    lv_label_set_text(day_label_, "--月--日 星期-");

    // 大字时钟：反色时间块（白字黑底条），横跨卡片内宽，与参考图一致
    lv_obj_t *time_block = lv_obj_create(clock_card);
    lv_obj_set_size(time_block, clock_w - 10, 56);
    lv_obj_align(time_block, LV_ALIGN_TOP_MID, 0, 30);
    lv_obj_set_style_bg_color(time_block, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(time_block, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(time_block, 0, 0);
    lv_obj_set_style_radius(time_block, 0, 0);
    lv_obj_set_style_pad_all(time_block, 0, 0);
    lv_obj_remove_flag(time_block, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(time_block, LV_OBJ_FLAG_CLICKABLE);

    time_label_ = lv_label_create(time_block);
    lv_obj_set_style_text_color(time_label_, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(time_label_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_font(time_label_, font_clock, 0);
    lv_obj_set_style_text_letter_space(time_label_, -2, 0);
    lv_obj_set_style_text_align(time_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(time_label_, LV_LABEL_LONG_CLIP);
    lv_obj_center(time_label_);
    // 诊断：打印真实文本宽度，确认不会折行
    lv_label_set_text(time_label_, "17:35");
    lv_obj_update_layout(time_label_);
    ESP_LOGI(TAG, "大字时间：文本 \"17:35\" 实测 %dx%d px，时间块可用宽 %d px",
             (int)lv_obj_get_width(time_label_), (int)lv_obj_get_height(time_label_), clock_w - 10);
    lv_label_set_text(time_label_, "00:00");

    // 天气行：「成都 晴 23°C」（黑字白底，卡片底部）
    weather_label_ = lv_label_create(clock_card);
    lv_obj_set_style_text_font(weather_label_, font_ai, 0);
    lv_obj_set_style_text_color(weather_label_, lv_color_black(), 0);
    lv_obj_set_style_text_align(weather_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(weather_label_, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_label_set_text(weather_label_, "-- --°C");

    // 内框装饰（双边框，和参考图一致）
    lv_obj_t *clock_inner = lv_obj_create(clock_card);
    lv_obj_set_size(clock_inner, clock_w - 12, clock_h - 12);
    lv_obj_center(clock_inner);
    lv_obj_set_style_bg_opa(clock_inner, 0, 0);
    lv_obj_set_style_border_width(clock_inner, 2, 0);
    lv_obj_set_style_border_color(clock_inner, lv_color_black(), 0);
    lv_obj_set_style_radius(clock_inner, 10, 0);
    lv_obj_remove_flag(clock_inner, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(clock_inner, LV_OBJ_FLAG_CLICKABLE);

    // ===== 待办卡片（右侧整高）=====
    lv_obj_t *todo_card = lv_obj_create(screen);
    lv_obj_set_pos(todo_card, todo_x, todo_y);
    lv_obj_set_size(todo_card, todo_w, todo_h);
    lv_obj_set_style_border_width(todo_card, 2, 0);
    lv_obj_set_style_border_color(todo_card, lv_color_black(), 0);
    lv_obj_set_style_radius(todo_card, 15, 0);
    lv_obj_set_style_bg_color(todo_card, lv_color_white(), 0);
    lv_obj_set_style_pad_all(todo_card, 0, 0);
    lv_obj_remove_flag(todo_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_clip_corner(todo_card, true, 0);

    lv_obj_t *todo_title = lv_label_create(todo_card);
    lv_obj_set_style_text_font(todo_title, font_ai, 0);
    lv_obj_set_style_text_color(todo_title, lv_color_black(), 0);
    lv_obj_set_style_text_letter_space(todo_title, 1, 0);
    lv_obj_align(todo_title, LV_ALIGN_TOP_LEFT, 12, 8);
    lv_label_set_text(todo_title, "待办");

    // 标题右侧两个图标（☑ ★），与参考图一致
    lv_obj_t *todo_icon_star = lv_label_create(todo_card);
    lv_obj_set_style_text_font(todo_icon_star, font_icon, 0);
    lv_obj_set_style_text_color(todo_icon_star, lv_color_black(), 0);
    lv_obj_align(todo_icon_star, LV_ALIGN_TOP_RIGHT, -12, 10);
    lv_label_set_text(todo_icon_star, MATERIAL_SYMBOLS_STAR);

    lv_obj_t *todo_icon_check = lv_label_create(todo_card);
    lv_obj_set_style_text_font(todo_icon_check, font_icon, 0);
    lv_obj_set_style_text_color(todo_icon_check, lv_color_black(), 0);
    lv_obj_align(todo_icon_check, LV_ALIGN_TOP_RIGHT, -34, 10);
    lv_label_set_text(todo_icon_check, MATERIAL_SYMBOLS_CHECK_CIRCLE);

    // 标题下分隔线
    lv_obj_t *todo_sep = lv_obj_create(todo_card);
    lv_obj_set_size(todo_sep, todo_w - 24, 2);
    lv_obj_set_style_bg_color(todo_sep, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(todo_sep, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(todo_sep, 0, 0);
    lv_obj_set_style_radius(todo_sep, 1, 0);
    lv_obj_align(todo_sep, LV_ALIGN_TOP_MID, 0, 38);
    lv_obj_remove_flag(todo_sep, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(todo_sep, LV_OBJ_FLAG_CLICKABLE);

    // 待办列表容器：每行 = ★ + 文本（文本自动换行）
    memo_rows_w_ = todo_w - 24;
    memo_rows_ = lv_obj_create(todo_card);
    lv_obj_set_pos(memo_rows_, 12, 46);
    lv_obj_set_size(memo_rows_, memo_rows_w_, todo_h - 54);
    lv_obj_set_style_bg_opa(memo_rows_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(memo_rows_, 0, 0);
    lv_obj_set_style_pad_all(memo_rows_, 0, 0);
    lv_obj_set_style_pad_row(memo_rows_, 4, 0);
    lv_obj_remove_flag(memo_rows_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(memo_rows_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(memo_rows_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    // ===== AI 对话卡片（左下）=====
    // 布局：[左侧表情区 64px | 分隔线 | 右侧对话文字区]
    const int emotion_w = 56;                                // 左侧表情区宽度（约占卡片 1/3）
    const bool compact_ai = (ai_h < 90);
    const int emo_img_size = compact_ai ? 30 : 48;
    const int emo_img_dy   = compact_ai ? -8 : -14;
    const int emo_lbl_dy   = compact_ai ? 14 : 28;
    const lv_font_t *emo_lbl_font = compact_ai ? font_tiny : font_ai;

    chat_card_ = lv_obj_create(screen);
    lv_obj_set_pos(chat_card_, ai_x, ai_y);
    lv_obj_set_size(chat_card_, ai_w, ai_h);
    lv_obj_set_style_border_width(chat_card_, 2, 0);
    lv_obj_set_style_border_color(chat_card_, lv_color_black(), 0);
    lv_obj_set_style_radius(chat_card_, 15, 0);
    lv_obj_set_style_bg_color(chat_card_, lv_color_white(), 0);
    lv_obj_set_style_pad_all(chat_card_, 0, 0);
    lv_obj_remove_flag(chat_card_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_clip_corner(chat_card_, true, 0);

    lv_obj_t *chat_inner = lv_obj_create(chat_card_);
    lv_obj_set_size(chat_inner, ai_w - 12, ai_h - 12);
    lv_obj_center(chat_inner);
    lv_obj_set_style_bg_opa(chat_inner, 0, 0);
    lv_obj_set_style_border_width(chat_inner, 2, 0);
    lv_obj_set_style_border_color(chat_inner, lv_color_black(), 0);
    lv_obj_set_style_radius(chat_inner, 10, 0);
    lv_obj_set_style_pad_all(chat_inner, 0, 0);
    lv_obj_remove_flag(chat_inner, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_clip_corner(chat_inner, true, 0);

    emotion_img_ = lv_image_create(chat_inner);
    lv_obj_set_size(emotion_img_, emo_img_size, emo_img_size);
    lv_image_set_inner_align(emotion_img_, LV_IMAGE_ALIGN_CENTER);
    lv_obj_align(emotion_img_, LV_ALIGN_LEFT_MID, 12, emo_img_dy);
    lv_obj_add_flag(emotion_img_, LV_OBJ_FLAG_HIDDEN);

    emotion_label_ = lv_label_create(chat_inner);
    lv_obj_set_style_text_font(emotion_label_, emo_lbl_font, 0);
    lv_obj_set_style_text_color(emotion_label_, lv_color_black(), 0);
    lv_obj_set_style_text_align(emotion_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(emotion_label_, emotion_w);
    lv_label_set_long_mode(emotion_label_, LV_LABEL_LONG_WRAP);
    lv_obj_align(emotion_label_, LV_ALIGN_LEFT_MID, 4, emo_lbl_dy);
    lv_label_set_text(emotion_label_, "待命");

    lv_obj_t *divider = lv_obj_create(chat_inner);
    lv_obj_set_size(divider, 2, ai_h - 28);
    lv_obj_set_style_bg_color(divider, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(divider, 0, 0);
    lv_obj_set_style_radius(divider, 1, 0);
    lv_obj_align(divider, LV_ALIGN_LEFT_MID, emotion_w + 14, 0);
    lv_obj_remove_flag(divider, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(divider, LV_OBJ_FLAG_CLICKABLE);

    int text_area_w = ai_w - emotion_w - 14 - 2 - 20;
    chat_status_label_ = lv_label_create(chat_inner);
    lv_obj_set_style_text_font(chat_status_label_, font_ai, 0);
    lv_obj_set_style_text_color(chat_status_label_, lv_color_black(), 0);
    lv_obj_set_style_text_align(chat_status_label_, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_width(chat_status_label_, text_area_w);
    lv_obj_set_style_text_line_space(chat_status_label_, 3, 0);
    lv_label_set_long_mode(chat_status_label_, LV_LABEL_LONG_WRAP);
    lv_obj_align(chat_status_label_, LV_ALIGN_LEFT_MID, emotion_w + 20, 0);
    lv_label_set_text(chat_status_label_, "AI 待命");

    // 给基类创建隐藏的占位控件（防止基类方法空指针崩溃）
    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, 1, 1);
    lv_obj_add_flag(container_, LV_OBJ_FLAG_HIDDEN);

    network_label_ = lv_label_create(screen);
    lv_label_set_text(network_label_, "");
    lv_obj_add_flag(network_label_, LV_OBJ_FLAG_HIDDEN);

    battery_label_ = lv_label_create(screen);
    lv_label_set_text(battery_label_, "");
    lv_obj_add_flag(battery_label_, LV_OBJ_FLAG_HIDDEN);

    status_label_ = lv_label_create(screen);
    lv_label_set_text(status_label_, "");
    lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN);

    notification_label_ = lv_label_create(screen);
    lv_label_set_text(notification_label_, "");
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    mute_label_ = lv_label_create(screen);
    lv_label_set_text(mute_label_, "");
    lv_obj_add_flag(mute_label_, LV_OBJ_FLAG_HIDDEN);

    low_battery_popup_ = lv_obj_create(screen);
    lv_obj_set_scrollbar_mode(low_battery_popup_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(low_battery_popup_, (UI_SCR_W - 20) < 320 ? (UI_SCR_W - 20) : 320, 42);
    lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_color(low_battery_popup_, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(low_battery_popup_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(low_battery_popup_, 2, 0);
    lv_obj_set_style_border_color(low_battery_popup_, lv_color_black(), 0);
    lv_obj_set_style_radius(low_battery_popup_, 12, 0);
    lv_obj_set_style_pad_all(low_battery_popup_, 6, 0);
    lv_obj_remove_flag(low_battery_popup_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
    low_battery_label_ = lv_label_create(low_battery_popup_);
    lv_obj_set_style_text_font(low_battery_label_, font_ai, 0);
    lv_obj_set_style_text_color(low_battery_label_, lv_color_black(), 0);
    lv_obj_set_style_text_align(low_battery_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(low_battery_label_, UI_SCR_W - 40);
    lv_obj_center(low_battery_label_);
    lv_label_set_text(low_battery_label_, "电量低，请尽快充电");

    // emoji 相关（SetEmotion 基类方法会用到）
    emoji_label_ = lv_label_create(screen);
    lv_label_set_text(emoji_label_, "");
    lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);

    emoji_image_ = lv_image_create(screen);
    lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);

    // chat_message_label_ 指向我们的 AI 状态标签（让基类方法能更新它）
    chat_message_label_ = chat_status_label_;

    // 默认「待命」表情图（参考界面左下常驻）
    ApplyDefaultEmotion();

    // 首次填充待办列表（已持锁，用 Internal 版本）
    RefreshMemoDisplayInternal();

    ESP_LOGI(TAG, "天气站 UI 创建完成（时钟卡 %dx%d, 待办卡 %dx%d, AI 卡 %dx%d）",
             clock_w, clock_h, todo_w, todo_h, ai_w, ai_h);
}
