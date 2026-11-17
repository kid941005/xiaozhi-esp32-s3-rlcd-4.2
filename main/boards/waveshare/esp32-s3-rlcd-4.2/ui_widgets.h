#pragma once
#include "lvgl.h"

// 状态栏右上角白底胶囊：WiFi 图标 + 电池图标 + 电量文字。
// 天气页与音乐页共用同一实现，仅尺寸/对齐/字体不同；输出参数用于回写各页成员变量，
// 不需要的可在调用处传 nullptr。
lv_obj_t *BuildStatusCapsule(lv_obj_t *parent,
                             int w, int h,
                             int align_x, int align_y,
                             const lv_font_t *font,
                             lv_obj_t **out_wifi,
                             lv_obj_t **out_battery,
                             lv_obj_t **out_pct);

// AI 对话卡片参数。天气页与音乐页布局略有差异，全部显式化以便共用 BuildAiCard()：
// [表情图/猫脸 + 表情文字] | [分隔线] | [AI 对话文字]
struct AiCardConfig {
    int x;                      // 卡片左上角 x
    int y;                      // 卡片左上角 y
    int w;                      // 卡片宽
    int h;                      // 卡片高
    int radius;                 // 外卡片圆角（天气 15 / 音乐 16）
    bool inner_frame;           // 是否加一层内边框（天气页有，音乐页无）
    int emotion_w;              // 左侧表情区宽度
    int emo_img_dx;             // 表情图/猫脸左对齐 x 偏移
    int emo_img_dy;             // 表情图/猫脸 y 偏移
    int emo_img_size;           // 表情图/猫脸边长
    int emo_lbl_dy;             // 表情文字 y 偏移
    const lv_font_t *emo_font;  // 表情文字字体
    const lv_font_t *chat_font; // AI 对话文字字体
    int divider_off;            // 分隔线 x = emotion_w + divider_off
    int divider_h_margin;       // 分隔线高 = h - divider_h_margin
    int text_x_off;             // 对话文字 x = emotion_w + text_x_off
    int text_right_margin;      // 对话文字右侧留白
};

// 创建 AI 对话卡片。返回外卡片对象；out_* 可为 nullptr。
lv_obj_t *BuildAiCard(lv_obj_t *parent, const AiCardConfig &cfg,
                      lv_obj_t **out_emotion_img,
                      lv_obj_t **out_ai_face,
                      lv_obj_t **out_emotion_label,
                      lv_obj_t **out_chat_status);
