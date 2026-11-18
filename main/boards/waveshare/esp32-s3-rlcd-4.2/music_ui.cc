// 音乐页 UI —— 沉浸式播放器布局（左右排布版）
//
// 400x300 黑白单色 RLCD，全屏黑底白字。
// 所有中文文字统一使用小智自带的 font_puhui_16_1_full（6656 字形全字库），避免缺字。
//
// 布局：
// ┌──────────────────────────────────────────┐
// │ 环境 24.6°C 40%   14:30   [WiFi][电池] │  顶部信息层
// │                                          │
// │  ┌──────────┐   晴天                     │
// │  │          │   周杰伦                   │
// │  │  ◉ 唱片  │                           │
// │  │ (120x120)│   故事的小黄花             │
// │  │          │   从出生那年就飘着          │
// │  └──────────┘                            │
// │                                          │
// │  ━━━━━━━━━━━━━━━━━━━━○──────────  02:31  │  进度条 + 时间
// │                                          │
// │  ┌──────────────────────────────────┐    │
// │  │[emoji]     │                     │    │  底部 AI 状态卡
// │  │ 待命       │ AI 待命              │    │  （和天气页一致的 emoji+文字 布局）
// │  └──────────────────────────────────┘    │
// └──────────────────────────────────────────┘

#include "custom_lcd_display.h"
#include "ui_cat_face.h"
#include "ui_widgets.h"
#include "ui_assets.h"
#include <esp_log.h>

static const char *TAG = "MusicUI";

void CustomLcdDisplay::SetupMusicUI() {
    DisplayLockGuard lock(this);

    lv_obj_t *root = lv_screen_active();
    const lv_font_t *font_num    = &alibaba_puhui_16;   // 纯数字/ASCII 用
    const lv_font_t *font_time   = &alibaba_puhui_24;   // 时钟数字
    const lv_font_t *font_cn     = &font_puhui_16_1_full;    // 所有中文文字统一用这个
    const lv_font_t *font_sm     = &font_puhui_14_1;    // 14px 小字
    const lv_font_t *font_static = &font_puhui_16_1;    // 16px 1bpp 点阵（内容固定的文案）

    // 全局布局常量
    const int SCR_W = UI_SCR_W;
    const int SCR_H = UI_SCR_H;
    const int PAD = 12;               // 左右安全边距

    // ===== 音乐页面容器（全屏黑底，初始隐藏）=====
    music_page_ = lv_obj_create(root);
    lv_obj_set_size(music_page_, SCR_W, SCR_H);
    lv_obj_set_pos(music_page_, 0, 0);
    lv_obj_set_style_bg_color(music_page_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(music_page_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(music_page_, 0, 0);
    lv_obj_set_style_pad_all(music_page_, 0, 0);
    lv_obj_set_style_radius(music_page_, 0, 0);
    lv_obj_remove_flag(music_page_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(music_page_, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *page = music_page_;

    // ============================================================
    // 第 1 层：顶部信息（时钟 + 温湿度 + 状态栏胶囊）
    // ============================================================

    // 左侧：环境温湿度（与天气页顶栏同一文案格式；16px 1bpp 点阵字体，1-bit 屏上笔画更实）
    music_sensor_label_ = lv_label_create(page);
    lv_obj_set_style_text_font(music_sensor_label_, font_static, 0);
    lv_obj_set_style_text_color(music_sensor_label_, lv_color_white(), 0);
    lv_obj_align(music_sensor_label_, LV_ALIGN_TOP_LEFT, 10, 9);
    lv_label_set_text(music_sensor_label_, "环境 --.-°C --%");

    // 中间：时钟（水平居中）
    music_time_label_ = lv_label_create(page);
    lv_obj_set_style_text_font(music_time_label_, font_time, 0);
    lv_obj_set_style_text_color(music_time_label_, lv_color_white(), 0);
    lv_obj_align(music_time_label_, LV_ALIGN_TOP_MID, 0, 6);
    lv_label_set_text(music_time_label_, "00:00");

    // 右上角状态栏胶囊（与天气页共用 BuildStatusCapsule）
    BuildStatusCapsule(page, 115, 28, -8, 4, font_num,
                       &music_wifi_icon_img_, &music_battery_icon_img_, &music_battery_pct_label_);

    // ============================================================
    // 第 2 层：主内容区 —— 左侧唱片 + 右侧歌曲信息（左右排布）
    // ============================================================

    const int content_y = 40;         // 顶部信息下方（给居中时钟留出高度）
#if UI_PORTRAIT
    const int vinyl_card_size = 110;  // 唱片方卡尺寸
    const int vinyl_size = 96;        // 唱片圆盘直径
#else
    const int vinyl_card_size = 150;  // 唱片方卡尺寸
    const int vinyl_size = 130;       // 唱片圆盘直径
#endif
    // 右侧信息卡高度：竖屏时高于唱片，给 3 行歌词留空间
    const int info_h = UI_PORTRAIT ? 150 : vinyl_card_size;
    const int vinyl_x = PAD;          // 唱片左边距
    const int info_x = vinyl_x + vinyl_card_size + 12;  // 右侧信息区起始 x
    const int info_w = SCR_W - info_x - PAD;             // 右侧信息区宽度

    // --- 左侧：唱片方卡 ---
    lv_obj_t *vinyl_card = lv_obj_create(page);
    lv_obj_set_size(vinyl_card, vinyl_card_size, vinyl_card_size);
    lv_obj_set_pos(vinyl_card, vinyl_x, content_y);
    lv_obj_set_style_bg_color(vinyl_card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(vinyl_card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(vinyl_card, 2, 0);
    lv_obj_set_style_border_color(vinyl_card, lv_color_black(), 0);
    lv_obj_set_style_radius(vinyl_card, 16, 0);
    lv_obj_set_style_pad_all(vinyl_card, 0, 0);
    lv_obj_remove_flag(vinyl_card, LV_OBJ_FLAG_SCROLLABLE);

    // 唱片圆盘（黑色）
    lv_obj_t *vinyl_disc = lv_obj_create(vinyl_card);
    lv_obj_set_size(vinyl_disc, vinyl_size, vinyl_size);
    lv_obj_center(vinyl_disc);
    lv_obj_set_style_radius(vinyl_disc, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(vinyl_disc, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(vinyl_disc, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(vinyl_disc, 2, 0);
    lv_obj_set_style_border_color(vinyl_disc, lv_color_white(), 0);
    lv_obj_set_style_pad_all(vinyl_disc, 0, 0);
    lv_obj_remove_flag(vinyl_disc, LV_OBJ_FLAG_SCROLLABLE);

    // 唱片纹路（3 层同心圆）
#if UI_PORTRAIT
    const int ring_sizes[] = {84, 66, 50};
#else
    const int ring_sizes[] = {104, 84, 64};
#endif
    for (int i = 0; i < 3; i++) {
        lv_obj_t *ring = lv_obj_create(vinyl_disc);
        lv_obj_set_size(ring, ring_sizes[i], ring_sizes[i]);
        lv_obj_center(ring);
        lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(ring, 1, 0);
        lv_obj_set_style_border_color(ring, lv_color_white(), 0);
        // 1-bit 屏上 40% 白（亮度 102 < 阈值 112）会被量化成黑，纹路直接消失，必须给足不透明度
        lv_obj_set_style_border_opa(ring, LV_OPA_COVER, 0);
        lv_obj_remove_flag(ring, LV_OBJ_FLAG_SCROLLABLE);
    }

    // 中心标签区（白色圆形）
#if UI_PORTRAIT
    const int center_size = 34;
#else
    const int center_size = 40;
#endif
    lv_obj_t *vinyl_center = lv_obj_create(vinyl_disc);
    lv_obj_set_size(vinyl_center, center_size, center_size);
    lv_obj_center(vinyl_center);
    lv_obj_set_style_radius(vinyl_center, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(vinyl_center, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(vinyl_center, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(vinyl_center, 2, 0);
    lv_obj_set_style_border_color(vinyl_center, lv_color_black(), 0);
    lv_obj_set_style_pad_all(vinyl_center, 0, 0);
    lv_obj_remove_flag(vinyl_center, LV_OBJ_FLAG_SCROLLABLE);

    // 中心小孔
    lv_obj_t *vinyl_hole = lv_obj_create(vinyl_center);
    lv_obj_set_size(vinyl_hole, 10, 10);
    lv_obj_center(vinyl_hole);
    lv_obj_set_style_radius(vinyl_hole, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(vinyl_hole, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(vinyl_hole, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(vinyl_hole, 0, 0);
    lv_obj_remove_flag(vinyl_hole, LV_OBJ_FLAG_SCROLLABLE);

    // --- 右侧：歌曲信息（白色卡片，和唱片等高）---
    lv_obj_t *info_card = lv_obj_create(page);
    lv_obj_set_size(info_card, info_w, info_h);
    lv_obj_set_pos(info_card, info_x, content_y);
    lv_obj_set_style_bg_color(info_card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(info_card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(info_card, 2, 0);
    lv_obj_set_style_border_color(info_card, lv_color_black(), 0);
    lv_obj_set_style_radius(info_card, 16, 0);
    lv_obj_set_style_pad_all(info_card, 10, 0);
    lv_obj_remove_flag(info_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_clip_corner(info_card, true, 0);

    // 歌名（小智完整字库，黑字，不会缺字）
    music_title_label_ = lv_label_create(info_card);
    lv_obj_set_style_text_font(music_title_label_, font_cn, 0);
    lv_obj_set_style_text_color(music_title_label_, lv_color_black(), 0);
    lv_obj_set_style_text_align(music_title_label_, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_width(music_title_label_, info_w - 24);
    lv_label_set_long_mode(music_title_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(music_title_label_, "未播放");
    lv_obj_align(music_title_label_, LV_ALIGN_TOP_LEFT, 0, 4);

    // 歌手（小字形成层级）：1-bit 屏无法表现半透明（LV_OPA_60 会被阈值吃掉），
    // 改用字号区分——歌手 14px < 歌名 16px，并保持完全不透明。
    music_artist_label_ = lv_label_create(info_card);
    lv_obj_set_style_text_font(music_artist_label_, font_sm, 0);
    lv_obj_set_style_text_color(music_artist_label_, lv_color_black(), 0);
    lv_obj_set_style_text_align(music_artist_label_, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_width(music_artist_label_, info_w - 24);
    lv_label_set_long_mode(music_artist_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(music_artist_label_, "未知歌手");
    lv_obj_align(music_artist_label_, LV_ALIGN_TOP_LEFT, 0, 28);

    // 分隔线
    lv_obj_t *info_sep = lv_obj_create(info_card);
    lv_obj_set_size(info_sep, info_w - 30, 1);
    lv_obj_set_style_bg_color(info_sep, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(info_sep, LV_OPA_COVER, 0);   // 同上：1-bit 下低不透明度等于不显示
    lv_obj_set_style_border_width(info_sep, 0, 0);
    lv_obj_set_style_radius(info_sep, 0, 0);
    lv_obj_align(info_sep, LV_ALIGN_TOP_LEFT, 0, 50);
    lv_obj_remove_flag(info_sep, LV_OBJ_FLAG_SCROLLABLE);

    // 歌词区域（三行卡拉 OK 样式：上一句淡色、当前句醒目、下一句淡色）
    const int lyric_start_y = 58;       // 分隔线下方起始 y
    const int lyric_line_h = 24;        // 每行歌词高度
    const int lyric_gap = 3;            // 行间距
    const int lyric_w = info_w - 24;

    // 上一句歌词（小字，单行省略号截断 —— 固定高度确保不换行）
    music_lyric_prev_label_ = lv_label_create(info_card);
    lv_obj_set_style_text_font(music_lyric_prev_label_, font_sm, 0);
    lv_obj_set_style_text_color(music_lyric_prev_label_, lv_color_black(), 0);
    lv_obj_set_style_text_align(music_lyric_prev_label_, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_size(music_lyric_prev_label_, lyric_w, lyric_line_h);
    lv_label_set_long_mode(music_lyric_prev_label_, LV_LABEL_LONG_DOT);
    lv_label_set_text(music_lyric_prev_label_, "");
    lv_obj_align(music_lyric_prev_label_, LV_ALIGN_TOP_LEFT, 0, lyric_start_y);

    // 当前歌词（完整字库，醒目黑色，加大字号）
    music_lyric_label_ = lv_label_create(info_card);
    lv_obj_set_style_text_font(music_lyric_label_, font_cn, 0);
    lv_obj_set_style_text_color(music_lyric_label_, lv_color_black(), 0);
    lv_obj_set_style_text_align(music_lyric_label_, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_width(music_lyric_label_, lyric_w);
    lv_label_set_long_mode(music_lyric_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(music_lyric_label_, "等待播放...");
    lv_obj_align(music_lyric_label_, LV_ALIGN_TOP_LEFT, 0, lyric_start_y + lyric_line_h + lyric_gap);

    // 下一句歌词（小字，单行省略号截断 —— 固定高度确保不换行）
    music_lyric_next_label_ = lv_label_create(info_card);
    lv_obj_set_style_text_font(music_lyric_next_label_, font_sm, 0);
    lv_obj_set_style_text_color(music_lyric_next_label_, lv_color_black(), 0);
    lv_obj_set_style_text_align(music_lyric_next_label_, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_size(music_lyric_next_label_, lyric_w, lyric_line_h);
    lv_label_set_long_mode(music_lyric_next_label_, LV_LABEL_LONG_DOT);
    lv_label_set_text(music_lyric_next_label_, "");
    lv_obj_align(music_lyric_next_label_, LV_ALIGN_TOP_LEFT, 0, lyric_start_y + (lyric_line_h + lyric_gap) * 2);

    // ============================================================
    // 第 3 层：进度条 + 时间
    // ============================================================

    const int bar_y = content_y + (info_h > vinyl_card_size ? info_h : vinyl_card_size) + 10;
    const int bar_w = SCR_W - PAD * 2 - 120;  // 留出右侧时间文字的空间

    music_progress_bar_ = lv_bar_create(page);
    // 高度加到 12px，内部留 2px padding 给白边
    lv_obj_set_size(music_progress_bar_, bar_w, 12);
    lv_obj_set_pos(music_progress_bar_, PAD, bar_y + 4);
    lv_bar_set_range(music_progress_bar_, 0, 1000);
    lv_bar_set_value(music_progress_bar_, 0, LV_ANIM_OFF);

    // ── 轨道（白色背景 + 白色边框）──
    // 在黑底屏上形成醒目的白色外壳
    lv_obj_set_style_bg_color(music_progress_bar_, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(music_progress_bar_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(music_progress_bar_, 1, 0);
    lv_obj_set_style_border_color(music_progress_bar_, lv_color_white(), 0);
    lv_obj_set_style_border_opa(music_progress_bar_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(music_progress_bar_, 6, 0);
    // 关键：给轨道加 padding，让黑色指示器内缩，四周露出白色底
    lv_obj_set_style_pad_top(music_progress_bar_, 2, 0);
    lv_obj_set_style_pad_bottom(music_progress_bar_, 2, 0);
    lv_obj_set_style_pad_left(music_progress_bar_, 2, 0);
    lv_obj_set_style_pad_right(music_progress_bar_, 2, 0);

    // ── 指示器（纯黑填充，无描边）──
    // 因为轨道有 padding，黑色指示器被白色包裹，自然形成白边
    lv_obj_set_style_bg_color(music_progress_bar_, lv_color_black(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(music_progress_bar_, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(music_progress_bar_, 4, LV_PART_INDICATOR);

    // 进度时间文字（进度条右侧）
    music_progress_label_ = lv_label_create(page);
    lv_obj_set_style_text_font(music_progress_label_, font_sm, 0);
    lv_obj_set_style_text_color(music_progress_label_, lv_color_white(), 0);
    // 1-bit 屏无法表现半透明（LV_OPA_70 会被阈值吃掉），改为完全不透明。
    lv_obj_set_style_text_opa(music_progress_label_, LV_OPA_COVER, 0);
    lv_label_set_text(music_progress_label_, "00:00 / 00:00");
    lv_obj_set_pos(music_progress_label_, PAD + bar_w + 8, bar_y + 2);

    // ============================================================
    // 第 4 层：底部 AI 状态卡（和天气页 AI 卡片一致的布局）
    // [emoji 图片] [表情文字] | [AI 对话文字]
    // ============================================================

    const int ai_h = 72;              // 比之前的 32px 加大，给 emoji 留空间
    const int ai_w = SCR_W - PAD * 2;
    const int ai_y = SCR_H - ai_h - 6;

    // 与天气页共用 BuildAiCard()，仅把两页不同的尺寸/字体/偏移显式参数化。
    AiCardConfig ai_cfg = {};
    ai_cfg.x = PAD;
    ai_cfg.y = ai_y;
    ai_cfg.w = ai_w;
    ai_cfg.h = ai_h;
    ai_cfg.radius = 16;
    ai_cfg.inner_frame = false;            // 音乐页无内层边框
    ai_cfg.emotion_w = 56;
    ai_cfg.emo_img_dx = 10;
    ai_cfg.emo_img_dy = -10;
    ai_cfg.emo_img_size = 40;
    ai_cfg.emo_lbl_dy = 20;
    ai_cfg.emo_font = font_cn;
    ai_cfg.chat_font = font_cn;
    ai_cfg.divider_off = 10;
    ai_cfg.divider_h_margin = 20;
    ai_cfg.text_x_off = 18;
    ai_cfg.text_right_margin = 12;
    BuildAiCard(page, ai_cfg, &music_emotion_img_, &music_ai_face_,
                &music_emotion_label_, &music_chat_status_label_);

    // 音乐页同样在无 emoji 素材时显示内置猫脸（此处已持锁，用不取锁的内部方法）
    ApplyDefaultEmotion();

    // 顶栏三块（左温湿度 / 中时钟 / 右胶囊）是否互相压字，靠实测宽度确认
    lv_obj_update_layout(page);   // 不先算布局，label 宽度读出来是 0
    ESP_LOGI(TAG, "音乐页顶栏：环境 %d px @x10，时钟 %d px 居中，胶囊 115 px（屏宽 %d）",
             lv_obj_get_width(music_sensor_label_), lv_obj_get_width(music_time_label_), SCR_W);
    ESP_LOGI(TAG, "音乐页面 UI 创建完成（左右排布 + AI 表情卡）");
}
