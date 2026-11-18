#ifndef __UI_ASSETS_H__
#define __UI_ASSETS_H__

// 板级 UI 共用的字体/图片资源声明（CODE_REVIEW P3 #9）
// 原先 weather_ui.cc / music_ui.cc / data_update_task.cc / ui_widgets.cc /
// detail_ui.cc / custom_lcd_display.cc 各自重复声明同一批资源，改字号或换资源时
// 容易漏改。这里集中声明，各 UI 文件只 include 本头文件。
#include <lvgl.h>

// 天气站自带字体（字符集有限，用于数字/时钟等）
LV_FONT_DECLARE(alibaba_puhui_16);
LV_FONT_DECLARE(alibaba_puhui_24);
LV_FONT_DECLARE(alibaba_puhui_48);
LV_FONT_DECLARE(alibaba_black_64);

// 小智自带字体（7415 常用汉字 / 1bpp 点阵 / 图标）
LV_FONT_DECLARE(font_puhui_16_1_full);
LV_FONT_DECLARE(font_puhui_14_1);
LV_FONT_DECLARE(font_puhui_16_1);
LV_FONT_DECLARE(font_material_symbols_16_4);

// 状态栏图标
LV_IMAGE_DECLARE(ui_img_wifi);
LV_IMAGE_DECLARE(ui_img_wifi_low);
LV_IMAGE_DECLARE(ui_img_wifi_off);
LV_IMAGE_DECLARE(ui_img_battery_full);
LV_IMAGE_DECLARE(ui_img_battery_medium);
LV_IMAGE_DECLARE(ui_img_battery_low);
LV_IMAGE_DECLARE(ui_img_battery_charging);

#endif  // __UI_ASSETS_H__
