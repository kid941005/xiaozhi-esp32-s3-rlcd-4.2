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

// ===== 天气 =====
// 优先使用和风天气（QWeather）：可拿到实时（体感/风/湿度/能见度/气压）+ 空气质量 +
// 3 天预报 + 日出日落 + 官方预警；凭证为空时自动回退免密钥源（中国天气网 → Open-Meteo）。
//
// 凭证填写优先级：NVS("weather"/"api_host","api_key") > managers/weather_qweather_secret.h > 这里
// secret 头文件已 gitignore，严禁把真实密钥写在本文件里。
#define QWEATHER_HOST ""   // 例："xxxxxx.re.qweatherapi.com"（控制台项目管理里的 API Host）
#define QWEATHER_KEY  ""   // 例：32 位十六进制 API KEY

// 免密钥源参数（和风不可用时的兜底）
#define WEATHER_LAT   30.6598f    // 纬度（默认成都）
#define WEATHER_LON   104.0633f   // 经度
#define WEATHER_CITY  "成都"       // 查询/显示的城市名（和风 geo 定位也用它）
#define WEATHER_CITY_CODE "101270101"

// IP 定位（ip-api.com 免费接口，HTTP，45 次/分钟）：开机定位一次，得到实际所在城市与经纬度
#define WEATHER_IP_GEO_URL "http://ip-api.com/json/?lang=zh-CN&fields=status,country,regionName,city,lat,lon"  // 中国天气网城市代码（成都），免密钥天气源使用

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
