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
#include "ui_cat_face.h"
#include "ui_widgets.h"
#include <esp_log.h>

// 天气站专用字体（从 MyWeatherStation 移植，字符集有限但够天气站用）
LV_FONT_DECLARE(alibaba_puhui_16);
LV_FONT_DECLARE(alibaba_puhui_24);
LV_FONT_DECLARE(alibaba_puhui_48);
LV_FONT_DECLARE(alibaba_black_64);

// 小智自带字体（7415 个常用汉字，用于中文文案）
LV_FONT_DECLARE(font_puhui_16_4);  // 16px 标准字体
LV_FONT_DECLARE(font_puhui_14_1);  // 14px 小字体
LV_FONT_DECLARE(font_puhui_16_1);  // 16px 1bpp 点阵（静态文案专用：无抗锯齿，二值化后不糊）
LV_FONT_DECLARE(font_material_symbols_16_4);  // 16px 图标字体（★ ☑）

// ===== 内置猫脸线稿（资源包没有 emoji 素材时的兜底图形）=====
// 全部用 lv_obj/lv_line 画，不依赖任何图片资源；坐标按 48x48 设计，按 size 等比缩放。
// lv_line_set_points 只保存指针、不拷贝点数组，因此每次调用单独分配点集，
// 并挂到 box 的 LV_EVENT_DELETE 上释放。若用文件级 static，会让天气页(48px)与
// 音乐页(40px)共享同一份坐标，后创建者覆盖前者，导致其中一页猫脸错位。
struct CatFacePoints {
    lv_point_precise_t ear_l[4];
    lv_point_precise_t ear_r[4];
    lv_point_precise_t mouth_v[2];
    lv_point_precise_t mouth_v2[3];
    lv_point_precise_t wsk[4][2];
};

static void CatFacePointsFree(lv_event_t *e) {
    void *p = lv_event_get_user_data(e);
    if (p) lv_free(p);
}

static lv_obj_t *CfLine(lv_obj_t *parent, lv_point_precise_t *pts, uint32_t n, int size) {
    lv_obj_t *line = lv_line_create(parent);
    lv_obj_set_size(line, size, size);
    lv_obj_set_pos(line, 0, 0);
    lv_obj_set_style_line_width(line, 2, 0);
    lv_obj_set_style_line_color(line, lv_color_black(), 0);
    lv_obj_set_style_line_rounded(line, true, 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(line, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(line, LV_OBJ_FLAG_CLICKABLE);
    lv_line_set_points(line, pts, n);
    return line;
}

static lv_obj_t *CfDot(lv_obj_t *parent, int x, int y, int w, int h) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

lv_obj_t *CreateCatFace(lv_obj_t *parent, int size) {
    const int s = size;
#define CF(v) ((lv_coord_t)((v) * s / 48))
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_set_size(box, s, s);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);

    CatFacePoints *pts = (CatFacePoints *)lv_malloc(sizeof(CatFacePoints));
    if (pts == nullptr) {
        ESP_LOGE("weather_ui", "猫脸点集分配失败（size=%d）", s);
        return box;
    }
    lv_obj_add_event_cb(box, CatFacePointsFree, LV_EVENT_DELETE, pts);

    // 脸（圆）
    const int face_d = CF(34);
    lv_obj_t *face = lv_obj_create(box);
    lv_obj_set_size(face, face_d, face_d);
    lv_obj_set_style_bg_opa(face, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(face, 2, 0);
    lv_obj_set_style_border_color(face, lv_color_black(), 0);
    lv_obj_set_style_radius(face, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(face, 0, 0);
    lv_obj_remove_flag(face, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(face, LV_ALIGN_BOTTOM_MID, 0, 0);

    // 耳朵
    pts->ear_l[0] = {CF(11), CF(18)};
    pts->ear_l[1] = {CF(8), CF(3)};
    pts->ear_l[2] = {CF(22), CF(12)};
    pts->ear_l[3] = {CF(11), CF(18)};
    pts->ear_r[0] = {CF(37), CF(18)};
    pts->ear_r[1] = {CF(40), CF(3)};
    pts->ear_r[2] = {CF(26), CF(12)};
    pts->ear_r[3] = {CF(37), CF(18)};
    CfLine(box, pts->ear_l, 4, s);
    CfLine(box, pts->ear_r, 4, s);

    // 眼睛
    CfDot(box, CF(16), CF(24), CF(4), CF(4));
    CfDot(box, CF(28), CF(24), CF(4), CF(4));

    // 鼻子 + 嘴
    CfDot(box, CF(22), CF(31), CF(4), CF(3));
    pts->mouth_v[0] = {CF(24), CF(34)};
    pts->mouth_v[1] = {CF(24), CF(36)};
    CfLine(box, pts->mouth_v, 2, s);
    pts->mouth_v2[0] = {CF(19), CF(39)};
    pts->mouth_v2[1] = {CF(24), CF(36)};
    pts->mouth_v2[2] = {CF(29), CF(39)};
    CfLine(box, pts->mouth_v2, 3, s);

    // 胡须（左右各两根）
    pts->wsk[0][0] = {CF(9), CF(26)};
    pts->wsk[0][1] = {CF(14), CF(27)};
    pts->wsk[1][0] = {CF(9), CF(31)};
    pts->wsk[1][1] = {CF(14), CF(30)};
    pts->wsk[2][0] = {CF(39), CF(26)};
    pts->wsk[2][1] = {CF(34), CF(27)};
    pts->wsk[3][0] = {CF(39), CF(31)};
    pts->wsk[3][1] = {CF(34), CF(30)};
    for (int i = 0; i < 4; i++) {
        CfLine(box, pts->wsk[i], 2, s);
    }
#undef CF
    return box;
}

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
    // 静态文案（内容固定的控件）用 16px 1bpp 点阵字体：字形本身就是二值点阵，
    // 不会像 4bpp 字体那样被 flush 阈值切出毛边，RLCD 上更清晰。
    const lv_font_t *font_static = &font_puhui_16_1;
    const lv_font_t *font_icon   = &font_material_symbols_16_4;

    // ===== 状态栏（右上角白底胶囊：WiFi + 电池 + 电量）=====
    // 与音乐页共用 BuildStatusCapsule()，仅尺寸/对齐/字体不同。
    BuildStatusCapsule(screen, 110, 24, -10, 9, font_small,
                       &wifi_icon_img_, &battery_icon_img_, &battery_pct_label_);

    // ===== 左上角「环境 温度 湿度」（白字，直接在黑底上）=====
    // 注意：必须用 font_puhui_16_4（7415 常用汉字），alibaba_puhui_16 字符集缺「环境」二字
    sensor_label_ = lv_label_create(screen);
    lv_obj_set_style_text_font(sensor_label_, font_static, 0);
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
    // AI 卡下边框必须与右栏待办卡下边框同高：两者底边都落在 UI_SCR_H - 10
    const int ai_h    = UI_SCR_H - ai_y - 10;                // 111
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
    lv_obj_set_style_text_font(day_label_, font_static, 0);
    lv_obj_set_style_text_color(day_label_, lv_color_black(), 0);
    lv_obj_align(day_label_, LV_ALIGN_TOP_MID, 0, 8);
    lv_label_set_text(day_label_, "--月--日 星期-");

    // 大字时钟：翻页时钟样式（每位一张黑底白字卡片 + 中缝 + 冒号），与微雪成品界面一致
    const int flip_card_w = 39;   // 单张数字卡片宽
    const int flip_card_h = 50;   // 单张数字卡片高
    const int flip_gap    = 3;    // 卡片间距
    const int colon_w     = 10;   // 冒号区宽
    const int flip_total  = flip_card_w * 4 + colon_w + flip_gap * 5;
    int flip_x = (clock_w - flip_total) / 2;
    const int flip_y = 30;

    for (int i = 0; i < 4; i++) {
        if (i == 2) {
            // 冒号：白底上的两个黑色小方块
            for (int d = 0; d < 2; d++) {
                lv_obj_t *dot = lv_obj_create(clock_card);
                lv_obj_set_size(dot, 5, 5);
                lv_obj_set_pos(dot, flip_x + (colon_w - 5) / 2, flip_y + (d == 0 ? 12 : 31));
                lv_obj_set_style_bg_color(dot, lv_color_black(), 0);
                lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
                lv_obj_set_style_border_width(dot, 0, 0);
                lv_obj_set_style_radius(dot, 2, 0);
                lv_obj_set_style_pad_all(dot, 0, 0);
                lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
            }
            flip_x += colon_w + flip_gap;
        }

        lv_obj_t *flip_card = lv_obj_create(clock_card);
        lv_obj_set_pos(flip_card, flip_x, flip_y);
        lv_obj_set_size(flip_card, flip_card_w, flip_card_h);
        lv_obj_set_style_bg_color(flip_card, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(flip_card, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(flip_card, 0, 0);
        lv_obj_set_style_radius(flip_card, 6, 0);
        lv_obj_set_style_pad_all(flip_card, 0, 0);
        lv_obj_remove_flag(flip_card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(flip_card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_clip_corner(flip_card, true, 0);

        lv_obj_t *digit = lv_label_create(flip_card);
        lv_obj_set_style_text_font(digit, font_clock, 0);
        lv_obj_set_style_text_color(digit, lv_color_white(), 0);
        lv_obj_set_style_text_letter_space(digit, 0, 0);
        lv_label_set_long_mode(digit, LV_LABEL_LONG_CLIP);
        lv_label_set_text(digit, "0");
        lv_obj_center(digit);
        time_digit_label_[i] = digit;

        // 翻页中缝：1px 白色横线，贯穿数字中部
        lv_obj_t *seam = lv_obj_create(flip_card);
        lv_obj_set_size(seam, flip_card_w, 1);
        lv_obj_set_style_bg_color(seam, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(seam, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(seam, 0, 0);
        lv_obj_set_style_radius(seam, 0, 0);
        lv_obj_set_style_pad_all(seam, 0, 0);
        lv_obj_align(seam, LV_ALIGN_CENTER, 0, 0);
        lv_obj_remove_flag(seam, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(seam, LV_OBJ_FLAG_CLICKABLE);

        flip_x += flip_card_w + flip_gap;
    }

    // 天气行：「成都 晴 23°C」（黑字白底，卡片底部）
    weather_label_ = lv_label_create(clock_card);
    lv_obj_set_style_text_font(weather_label_, font_ai, 0);
    lv_obj_set_style_text_color(weather_label_, lv_color_black(), 0);
    lv_obj_set_style_text_align(weather_label_, LV_TEXT_ALIGN_CENTER, 0);
    // 固定宽度 + 裁剪：这一行会轮播「实时/AQI/风力/日出日落/明日」，宽度必须锁在卡内
    lv_obj_set_width(weather_label_, clock_w - 20);
    lv_label_set_long_mode(weather_label_, LV_LABEL_LONG_CLIP);
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
    lv_obj_set_style_text_font(todo_title, font_static, 0);
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
    lv_obj_set_pos(memo_rows_, 12, 44);
    lv_obj_set_size(memo_rows_, memo_rows_w_, todo_h - 50);
    lv_obj_set_style_bg_opa(memo_rows_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(memo_rows_, 0, 0);
    lv_obj_set_style_pad_all(memo_rows_, 0, 0);
    lv_obj_set_style_pad_row(memo_rows_, 3, 0);
    lv_obj_remove_flag(memo_rows_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(memo_rows_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(memo_rows_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    // ===== AI 对话卡片（左下）=====
    // 布局：[左侧表情区 56px | 分隔线 | 右侧对话文字区]
    // 与音乐页共用 BuildAiCard()，仅把两页不同的尺寸/字体/偏移显式参数化。
    const bool compact_ai = (ai_h < 90);
    AiCardConfig ai_cfg = {};
    ai_cfg.x = ai_x;
    ai_cfg.y = ai_y;
    ai_cfg.w = ai_w;
    ai_cfg.h = ai_h;
    ai_cfg.radius = 15;
    ai_cfg.inner_frame = true;              // 天气页保留内层边框（双边框观感）
    ai_cfg.emotion_w = 56;
    ai_cfg.emo_img_dx = 12;
    ai_cfg.emo_img_dy = compact_ai ? -8 : -14;
    ai_cfg.emo_img_size = compact_ai ? 30 : 48;
    ai_cfg.emo_lbl_dy = compact_ai ? 14 : 28;
    ai_cfg.emo_font = compact_ai ? font_tiny : font_static;
    ai_cfg.chat_font = font_ai;
    ai_cfg.divider_off = 14;
    ai_cfg.divider_h_margin = 28;
    ai_cfg.text_x_off = 20;
    ai_cfg.text_right_margin = 16;
    chat_card_ = BuildAiCard(screen, ai_cfg, &emotion_img_, &ai_face_, &emotion_label_,
                             &chat_status_label_);

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
    lv_obj_set_style_text_font(low_battery_label_, font_static, 0);
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

    // 基类的 chat_message_label_ 用独立隐藏标签承接，避免基类直接写空串把我们的状态文案清掉
    // （小智 SetChatMessage 已被本类重写，真正显示走 chat_status_label_）
    chat_message_label_ = lv_label_create(screen);
    lv_label_set_text(chat_message_label_, "");
    lv_obj_add_flag(chat_message_label_, LV_OBJ_FLAG_HIDDEN);

    // 默认「待命」表情图（参考界面左下常驻）
    ApplyDefaultEmotion();

    // 首次填充待办列表（已持锁，用 Internal 版本）
    RefreshMemoDisplayInternal();

    ESP_LOGI(TAG, "天气站 UI 创建完成（时钟卡 %dx%d, 待办卡 %dx%d, AI 卡 %dx%d）",
             clock_w, clock_h, todo_w, todo_h, ai_w, ai_h);
}
