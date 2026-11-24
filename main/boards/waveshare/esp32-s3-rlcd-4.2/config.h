#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>
// 自包含所需类型/常量头（P2 #22），避免依赖包含顺序（不改变任何数值）
#include <driver/i2c_types.h>    // I2C_NUM_0
#include <driver/spi_common.h>   // SPI3_HOST
#include <es8311_codec.h>        // ES8311_CODEC_DEFAULT_ADDR
#include <es7210_adc.h>          // ES7210_CODEC_DEFAULT_ADDR

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
// 天气数据统一由 reminder MCP 服务的 get_weather 工具提供（与待办共用一条长连接），
// 设备不再直连 QWeather / 中国天气网 / Open-Meteo，也不做 IP 或后端地理定位。
// 手动城市通过 MCP self.weather.set_city 保存到 NVS weather/city。

// 待办 MCP 接入点（websocket JSON-RPC）。留空表示不启用，可在运行时通过
// MCP 工具 self.todo.set_url 配置（持久化在 NVS namespace "todo", key "mcp_url"）。
// 例：wss://example.com:59826/mcp_endpoint/call/?token=xxx
#define MCP_TODO_URL ""

// ===== 电池检测 =====
// 采样：ADC1 通道 3（ESP32-S3 上 ADC1_CH3 = GPIO4），12 dB 衰减、12 bit、硬件曲线拟合校准。
// 原理图已确认：R21=200K ±1% 接 VBAT，R23=100K ±1% 到 GND，分压比 3.0；充电芯片 STAT 仅接指示灯，不接 SoC。
// 分压比若与原理图不符，只改 BATTERY_ADC_DIVIDER 即可（电池电压 = ADC 引脚电压 × 该系数）。
#define BATTERY_ADC_DIVIDER   3.0f    // 分压系数（原理图 R21=200K / R23=100K，实测校准：4.2V 电池对应 ADC ≈1.40V）
#define BATTERY_ADC_SAMPLES   16      // 过采样次数（去最大最小后平均）
#define BATTERY_ADC_OFFSET_MV 0       // 电压校准偏移（mV）：万用表读数比固件高 X 时填 +X，反之填 -X；默认 0
#define BATTERY_LOAD_COMP_MV  30.0f   // 放电时的负载压降补偿（mV），充电时不补偿
#define BATTERY_FULL_MV       4180    // 满电/接 USB 平台电压（持续≥该值视为充电/已充满）
#define BATTERY_MIN_VALID_MV  2500    // 低于该值视为未接电池/采样无效，不显示电量
// 待补充（需硬件规格确认）：电池标称容量与整机平均电流。填 0 时界面/接口只显示百分比，
// 不输出「剩余 xx mAh / 预计可用 xx 小时」，避免用假设值误导。
#define BATTERY_CAPACITY_MAH  0       // 例如 2000（单节锂电）
#define BATTERY_AVG_CURRENT_MA 0      // 例如 120（整机平均放电电流）

#define BOOT_BUTTON_GPIO        GPIO_NUM_0   // 右侧按键：单击音量升一档（20/40/60/80/100 循环），双击降一档，长按静音/取消静音
#define USER_BUTTON_GPIO        GPIO_NUM_18  // 左侧按键：单击主页/天气/音乐页切换


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
