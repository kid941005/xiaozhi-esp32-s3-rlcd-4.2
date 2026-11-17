// 内置猫脸线稿（资源包没有 emoji 素材时的兜底图形）
//
// 实现位于 weather_ui.cc（用 lv_obj 画圆脸/眼睛/鼻子 + lv_line 画耳朵/嘴/胡须），
// 不依赖任何图片资源。RLCD 只有黑白两色，线稿正合适。
#pragma once

#include "lvgl.h"

// parent 下创建一个边长 size 的猫脸（内部按 48x48 设计等比缩放）
lv_obj_t *CreateCatFace(lv_obj_t *parent, int size);
