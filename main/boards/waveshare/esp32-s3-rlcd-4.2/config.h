#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

#define ESP32_I2C_HOST      I2C_NUM_0
#define ESP32_LCD_HOST      SPI3_HOST

#define AUDIO_INPUT_SAMPLE_RATE  24000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

#define AUDIO_INPUT_REFERENCE    true

#define AUDIO_I2S_GPIO_MCLK  GPIO_NUM_16
#define AUDIO_I2S_GPIO_WS    GPIO_NUM_45
#define AUDIO_I2S_GPIO_BCLK  GPIO_NUM_9
#define AUDIO_I2S_GPIO_DIN   GPIO_NUM_10
#define AUDIO_I2S_GPIO_DOUT  GPIO_NUM_8

#define AUDIO_CODEC_PA_PIN       GPIO_NUM_46
#define AUDIO_CODEC_I2C_SDA_PIN  GPIO_NUM_13
#define AUDIO_CODEC_I2C_SCL_PIN  GPIO_NUM_14
#define AUDIO_CODEC_ES8311_ADDR  ES8311_CODEC_DEFAULT_ADDR
#define AUDIO_CODEC_ES7210_ADDR  ES7210_CODEC_DEFAULT_ADDR

// ===== 天气（免密钥 Open-Meteo 源，未配置和风 Key 时使用）=====
#define WEATHER_LAT   30.6598f    // 纬度（默认成都）
#define WEATHER_LON   104.0633f   // 经度
#define WEATHER_CITY  "成都"       // 界面显示的城市名
#define WEATHER_CITY_CODE "101270101"  // 中国天气网城市代码（成都），免密钥天气源使用

// 待办 MCP 接入点（websocket JSON-RPC）。留空表示不启用，可在运行时通过
// MCP 工具 self.todo.set_url 配置（持久化在 NVS namespace "todo", key "mcp_url"）。
// 例：wss://example.com:59826/mcp_endpoint/call/?token=xxx
#define MCP_TODO_URL ""

#define BOOT_BUTTON_GPIO        GPIO_NUM_0
#define USER_BUTTON_GPIO        GPIO_NUM_18  // 用户按键（屏幕右侧 KEY 按钮）


#define RLCD_DC_PIN    GPIO_NUM_5  
#define RLCD_CS_PIN    GPIO_NUM_40
#define RLCD_SCK_PIN   GPIO_NUM_11
#define RLCD_MOSI_PIN  GPIO_NUM_12
#define RLCD_RST_PIN   GPIO_NUM_41
#define RLCD_TE_PIN    GPIO_NUM_6

// 屏幕方向：0 = 横屏 400x300（默认，2x2 卡片网格）；1 = 竖屏 300x400（单列堆叠）
// 改这一个宏即可切换，重新编译烧录生效
#define RLCD_PORTRAIT 0

#if RLCD_PORTRAIT
#define RLCD_WIDTH   300
#define RLCD_HEIGHT  400
#else
#define RLCD_WIDTH   400
#define RLCD_HEIGHT  300
#endif

#define DISPLAY_MIRROR_X false
#define DISPLAY_MIRROR_Y false
#define DISPLAY_SWAP_XY  false

#define DISPLAY_OFFSET_X  0
#define DISPLAY_OFFSET_Y  0

#endif 
