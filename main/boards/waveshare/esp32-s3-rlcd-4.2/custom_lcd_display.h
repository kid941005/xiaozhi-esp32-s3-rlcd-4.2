#ifndef __CUSTOM_LCD_DISPLAY_H__
#define __CUSTOM_LCD_DISPLAY_H__

#include <atomic>
#include <driver/gpio.h>
#include "config.h"
#include "lcd_display.h"

// 统一屏幕尺寸/方向宏，供各 UI 页面使用（由 config.h 的 RLCD_PORTRAIT 决定）
#define UI_SCR_W   RLCD_WIDTH
#define UI_SCR_H   RLCD_HEIGHT
#define UI_PORTRAIT (RLCD_HEIGHT > RLCD_WIDTH)
#include "rlcd_driver.h"
#include "managers/sensor_manager.h"
#include "managers/weather_manager.h"

// 天气站 + AI 混合显示
// 
// 屏幕布局 (400x300, 1-bit 单色 RLCD)，按微雪成品界面实测比例复刻：
// 环境 32.1°C 39%        十月初八 ╭─WiFi─▭─99%─╮
// ┌─────────────────┐ ┌─────────────┐
// │ 10月2日 星期五   │ │ 待办    ☑ ★ │
// │ ██ 17:18 ██(反色)│ │ ─────────── │
// │ 成都 晴间多云 23°C│ │ ★ 抢火车票  │
// ├─────────────────┤ │ ★ 记得吃饭  │
// │ AI 卡(199x100)  │ │             │
// └─────────────────┘ └─────────────┘
//   时钟卡 199x120      待办卡 173x249
// 顶栏浮在最上方：左「环境 温度 湿度」，右农历（14px）+ WiFi/电池胶囊
// 时间行是反色块（白字黑底），横跨时钟卡内宽
// 竖屏 300x400：时钟卡 → 待办卡 → AI 卡 整宽纵向堆叠
//
// 代码拆分为多个文件：
//   rlcd_driver.h/cc      - RLCD 硬件驱动层（SPI、像素映射、初始化命令）
//   weather_ui.cc          - 天气站 UI 布局（SetupWeatherUI）
//   music_ui.cc            - 音乐页 UI 布局（SetupMusicUI）
//   data_update_task.cc    - 后台数据更新任务（时间/天气/传感器/电池/WiFi/AI状态）
//   custom_lcd_display.cc  - 核心类（构造/析构/AI适配/备忘录/基类重写）
class CustomLcdDisplay : public LcdDisplay {
private:
    enum DisplayMode {
        MODE_WEATHER = 0,
        MODE_MUSIC = 1,
    };
    DisplayMode display_mode_ = MODE_WEATHER;

    // RLCD 硬件驱动（独立模块，负责 SPI 通信和像素操作）
    RlcdDriver *rlcd_ = nullptr;

    // ===== 1-bit 脏区局部刷新状态 =====
    // 一帧内可能多次 flush，先按 x 条带累积，帧末（flush_is_last）统一提交
    struct FlushRange {
        int x1;
        int x2;
    };
    static constexpr int kMaxFlushRanges = 6;   // 超过就退化为整帧
    static constexpr int kFlushMergeGap = 8;    // 相邻条带间隙小于该值则合并
    FlushRange flush_ranges_[kMaxFlushRanges] = {};
    int flush_range_count_ = 0;
    bool flush_force_full_ = false;
    bool flush_first_frame_ = true;   // 首帧整屏刷新，保证面板与影子缓冲一致

    // 刷新统计（帧数 / 整帧 / 局部 / 跳过 / 累计字节 / 累计传输耗时）
    uint32_t stat_frames_ = 0;
    uint32_t stat_full_ = 0;
    uint32_t stat_partial_ = 0;
    uint32_t stat_skip_ = 0;
    uint64_t stat_bytes_ = 0;
    uint64_t stat_transfer_us_ = 0;

    void FlushAddRange(int x1, int x2);   // 累积/合并脏区条带
    void FlushCommit();                   // 帧末提交：整帧或逐条带局部刷新 + 统计
    lv_obj_t *weather_page_ = nullptr;
    lv_obj_t *music_page_ = nullptr;

    // ===== 天气站 UI 组件 =====
    // 状态栏（右上角浮动胶囊）
    lv_obj_t *sensor_label_ = nullptr;      // 左上角温湿度标签
    
    // 时钟卡片（左上/竖屏顶部）
    lv_obj_t *day_label_ = nullptr;         // 日期行 "10月2日 星期五"
    lv_obj_t *time_label_ = nullptr;        // 大字时钟 "14:30"
    lv_obj_t *lunar_label_ = nullptr;       // 农历/节气/节日 "十月初七 小雪"
    lv_obj_t *weather_label_ = nullptr;     // 天气行 "成都 晴 23°C"
    
    // AI 对话卡片（左下）
    lv_obj_t *chat_card_ = nullptr;         // AI 卡片容器
    lv_obj_t *chat_status_label_ = nullptr; // AI 对话文字（右侧）
    lv_obj_t *emotion_label_ = nullptr;     // 表情文字（左侧下方）
    lv_obj_t *emotion_img_ = nullptr;       // 表情图片（左侧上方，小智自带 emoji）

    // 待办卡片（右侧整高）
    lv_obj_t *memo_rows_ = nullptr;           // 待办列表容器（每行 = ★ + 文本）
    int memo_rows_w_ = 160;                   // 待办行可用宽度（布局时确定）
    lv_obj_t *music_title_label_ = nullptr;   // 音乐页面：歌名
    lv_obj_t *music_artist_label_ = nullptr;  // 音乐页面：歌手
    lv_obj_t *music_lyric_prev_label_ = nullptr;  // 音乐页面：上一句歌词（淡色）
    lv_obj_t *music_lyric_label_ = nullptr;       // 音乐页面：当前歌词（醒目）
    lv_obj_t *music_lyric_next_label_ = nullptr;  // 音乐页面：下一句歌词（淡色）
    lv_obj_t *music_progress_bar_ = nullptr;  // 音乐页面：进度条
    lv_obj_t *music_progress_label_ = nullptr; // 音乐页面：进度文本
    lv_obj_t *music_time_label_ = nullptr;    // 音乐页面：左上角时钟
    lv_obj_t *music_sensor_label_ = nullptr;  // 音乐页面：左上角温湿度
    lv_obj_t *music_chat_status_label_ = nullptr; // 音乐页面：小智状态文案
    lv_obj_t *music_emotion_label_ = nullptr; // 音乐页面：小智情绪标签
    lv_obj_t *music_emotion_img_ = nullptr;   // 音乐页面：小智表情图片（和天气页 emotion_img_ 对应）

    // 图片图标（不能用基类的 label，因为我们用 lv_image 而不是 Font Awesome 文字）
    lv_obj_t *wifi_icon_img_ = nullptr;
    lv_obj_t *battery_icon_img_ = nullptr;
    lv_obj_t *battery_pct_label_ = nullptr;  // 电池百分比文字
    lv_obj_t *music_wifi_icon_img_ = nullptr;     // 音乐页面状态栏 WiFi 图标
    lv_obj_t *music_battery_icon_img_ = nullptr;  // 音乐页面状态栏电池图标
    lv_obj_t *music_battery_pct_label_ = nullptr; // 音乐页面状态栏电量文字

    // 数据更新任务句柄
    TaskHandle_t update_task_handle_ = nullptr;
    
    // 系统信息滚动标志（为 true 时暂停 DataUpdateTask 更新，避免锁竞争）
    std::atomic<bool> showing_system_info_{false};
    
    // 省电模式：5 分钟无活动后降低刷新频率（1秒 → 5秒）
    std::atomic<bool> power_saving_{false};     // 是否处于省电模式
    uint32_t last_activity_ms_ = 0;             // 上次用户活动的时间（tick 毫秒）
    static const uint32_t IDLE_TIMEOUT_MS = 5 * 60 * 1000;  // 5 分钟无活动进入省电
    static const int NORMAL_REFRESH_MS = 1000;  // 正常刷新间隔 1 秒
    static const int SAVING_REFRESH_MS = 5000;  // 省电刷新间隔 5 秒
    
    // 上次更新的值（用于避免不必要的 UI 刷新）
    int last_min_ = -1;
    time_t last_valid_epoch_ = 0;  // NTP 同步后记录正确的 epoch，用于检测时间被外部篡改
    float last_temp_ = -99.0f;
    float last_humi_ = -99.0f;

    // LVGL flush 回调（将 RGB565 转换为 1-bit 并刷新到 RLCD）
    static void Lvgl_flush_cb(lv_display_t * disp, const lv_area_t * area, uint8_t * color_p);

    // UI 创建（实现在 weather_ui.cc / music_ui.cc）
    void SetupWeatherUI();
    void SetupMusicUI();
    void ApplyDisplayMode();
    
    // 备忘录
    void LoadMemoFromNvs();   // 从 NVS 加载备忘录到 UI
    
    // 数据更新任务（实现在 data_update_task.cc）
    static void DataUpdateTask(void *arg);

public:
    CustomLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                  int width, int height, int offset_x, int offset_y,
                  bool mirror_x, bool mirror_y, bool swap_xy, spi_display_config_t spiconfig, spi_host_device_t spi_host = SPI3_HOST);
    ~CustomLcdDisplay();

    // 获取 RLCD 驱动（供外部调用硬件方法，如对比度调节）
    RlcdDriver* rlcd() const { return rlcd_; }
    
    // 获取 AI 对话标签（供 CustomBoard 设置滚动模式显示系统信息等）
    lv_obj_t* GetChatStatusLabel() const { return chat_status_label_; }
    
    // 系统信息滚动控制（供 CustomBoard 设置标志，避免 DataUpdateTask 锁竞争）
    void SetShowingSystemInfo(bool showing) { showing_system_info_ = showing; }
    
    // 省电模式：记录用户活动，唤醒省电模式
    void NotifyUserActivity();
    bool IsPowerSaving() const { return power_saving_; }
    
    // 重写小智的 AI 显示方法，适配到左下角卡片
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual bool SetEmotion(const char* emotion) override;
    virtual void ClearChatMessages() override;
    
    // 重写状态栏更新（我们用图片图标，不用 Font Awesome 文字）
    virtual void UpdateStatusBar(bool update_all = false) override;
    
    // 重写主题切换（RLCD 单色屏不需要主题切换，避免基类操作不存在的控件导致崩溃）
    virtual void SetTheme(Theme* theme) override;
    virtual void SetMusicInfo(const char* title, const char* artist) override;
    virtual void SetMusicLyric(const char* lyric) override;
    virtual void SetMusicProgress(uint32_t current_ms, uint32_t total_ms) override;
    virtual void SwitchToMusicPage() override;
    virtual void SwitchToWeatherPage() override;
    
    // 启动数据更新任务（需要在网络连接后调用）
    void StartDataUpdateTask();
    
    // 刷新右下角备忘录列表显示（从 NVS 读取后格式化显示）
    void RefreshMemoDisplay();           // 自动获取锁（外部调用用这个）
    void RefreshMemoDisplayInternal();   // 不获取锁（已持锁时用这个，避免死锁）
    void ApplyDefaultEmotion();          // 应用默认「待命」表情图（不获取锁）
    void CycleDisplayMode();
    bool IsMusicMode() const { return display_mode_ == MODE_MUSIC; }
};

#endif
