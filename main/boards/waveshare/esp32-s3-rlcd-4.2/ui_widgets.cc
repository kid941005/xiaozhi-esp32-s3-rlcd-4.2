// 天气页 / 音乐页共用控件工厂（状态栏白胶囊、AI 对话卡片）
// 抽取自 weather_ui.cc 与 music_ui.cc 中逐行重复的布局代码，行为保持一致。
#include "ui_widgets.h"

#include "ui_cat_face.h"

LV_IMAGE_DECLARE(ui_img_wifi_off);
LV_IMAGE_DECLARE(ui_img_battery_full);

lv_obj_t *BuildStatusCapsule(lv_obj_t *parent,
                             int w, int h,
                             int align_x, int align_y,
                             const lv_font_t *font,
                             lv_obj_t **out_wifi,
                             lv_obj_t **out_battery,
                             lv_obj_t **out_pct) {
    lv_obj_t *status_bar = lv_obj_create(parent);
    lv_obj_set_size(status_bar, w, h);
    lv_obj_set_style_bg_opa(status_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(status_bar, lv_color_white(), 0);
    lv_obj_set_style_border_width(status_bar, 0, 0);
    lv_obj_set_style_radius(status_bar, 14, 0);
    lv_obj_align(status_bar, LV_ALIGN_TOP_RIGHT, align_x, align_y);
    lv_obj_set_style_pad_all(status_bar, 0, 0);
    lv_obj_set_style_pad_left(status_bar, 8, 0);
    lv_obj_set_style_pad_right(status_bar, 8, 0);
    lv_obj_remove_flag(status_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(status_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_bar, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(status_bar, 5, 0);

    lv_obj_t *wifi = lv_image_create(status_bar);
    lv_image_set_src(wifi, &ui_img_wifi_off);

    lv_obj_t *batt = lv_image_create(status_bar);
    lv_image_set_src(batt, &ui_img_battery_full);

    lv_obj_t *pct = lv_label_create(status_bar);
    lv_obj_set_style_text_font(pct, font, 0);
    lv_obj_set_style_text_color(pct, lv_color_black(), 0);
    lv_label_set_text(pct, "---%");

    if (out_wifi) *out_wifi = wifi;
    if (out_battery) *out_battery = batt;
    if (out_pct) *out_pct = pct;
    return status_bar;
}

lv_obj_t *BuildAiCard(lv_obj_t *parent, const AiCardConfig &cfg,
                      lv_obj_t **out_emotion_img,
                      lv_obj_t **out_ai_face,
                      lv_obj_t **out_emotion_label,
                      lv_obj_t **out_chat_status) {
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_pos(card, cfg.x, cfg.y);
    lv_obj_set_size(card, cfg.w, cfg.h);
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_border_color(card, lv_color_black(), 0);
    lv_obj_set_style_radius(card, cfg.radius, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_clip_corner(card, true, 0);

    // 天气页需要一层内边框（双边框），音乐页没有；内层作为子控件的父容器
    lv_obj_t *content = card;
    if (cfg.inner_frame) {
        content = lv_obj_create(card);
        lv_obj_set_size(content, cfg.w - 12, cfg.h - 12);
        lv_obj_center(content);
        lv_obj_set_style_bg_opa(content, 0, 0);
        lv_obj_set_style_border_width(content, 2, 0);
        lv_obj_set_style_border_color(content, lv_color_black(), 0);
        lv_obj_set_style_radius(content, 10, 0);
        lv_obj_set_style_pad_all(content, 0, 0);
        lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_clip_corner(content, true, 0);
    }

    // 表情图（默认隐藏，等 SetEmotion 设置）
    lv_obj_t *emo_img = lv_image_create(content);
    lv_obj_set_size(emo_img, cfg.emo_img_size, cfg.emo_img_size);
    lv_image_set_inner_align(emo_img, LV_IMAGE_ALIGN_CENTER);
    lv_obj_align(emo_img, LV_ALIGN_LEFT_MID, cfg.emo_img_dx, cfg.emo_img_dy);
    lv_obj_add_flag(emo_img, LV_OBJ_FLAG_HIDDEN);

    // 内置猫脸线稿：资源包不带 emoji 素材时兜底
    lv_obj_t *ai_face = CreateCatFace(content, cfg.emo_img_size);
    lv_obj_align(ai_face, LV_ALIGN_LEFT_MID, cfg.emo_img_dx, cfg.emo_img_dy);

    // 表情文字（左侧下方）
    lv_obj_t *emo_lbl = lv_label_create(content);
    lv_obj_set_style_text_font(emo_lbl, cfg.emo_font, 0);
    lv_obj_set_style_text_color(emo_lbl, lv_color_black(), 0);
    lv_obj_set_style_text_align(emo_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(emo_lbl, cfg.emotion_w);
    lv_label_set_long_mode(emo_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_align(emo_lbl, LV_ALIGN_LEFT_MID, 4, cfg.emo_lbl_dy);
    lv_label_set_text(emo_lbl, "待命");

    // 竖分隔线
    lv_obj_t *divider = lv_obj_create(content);
    lv_obj_set_size(divider, 2, cfg.h - cfg.divider_h_margin);
    lv_obj_set_style_bg_color(divider, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(divider, 0, 0);
    lv_obj_set_style_radius(divider, 1, 0);
    lv_obj_align(divider, LV_ALIGN_LEFT_MID, cfg.emotion_w + cfg.divider_off, 0);
    lv_obj_remove_flag(divider, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(divider, LV_OBJ_FLAG_CLICKABLE);

    // 右侧 AI 对话文字
    const int text_x = cfg.emotion_w + cfg.text_x_off;
    const int text_w = cfg.w - text_x - cfg.text_right_margin;
    lv_obj_t *chat = lv_label_create(content);
    lv_obj_set_style_text_font(chat, cfg.chat_font, 0);
    lv_obj_set_style_text_color(chat, lv_color_black(), 0);
    lv_obj_set_style_text_align(chat, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_width(chat, text_w);
    lv_obj_set_style_text_line_space(chat, 3, 0);
    lv_label_set_long_mode(chat, LV_LABEL_LONG_WRAP);
    lv_obj_align(chat, LV_ALIGN_LEFT_MID, text_x, 0);
    lv_label_set_text(chat, "AI 待命");

    if (out_emotion_img) *out_emotion_img = emo_img;
    if (out_ai_face) *out_ai_face = ai_face;
    if (out_emotion_label) *out_emotion_label = emo_lbl;
    if (out_chat_status) *out_chat_status = chat;
    return card;
}
