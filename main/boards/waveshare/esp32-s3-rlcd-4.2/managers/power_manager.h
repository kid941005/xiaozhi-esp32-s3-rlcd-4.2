#pragma once

#include <esp_err.h>
#include <esp_pm.h>

/**
 * 板级电源管理：DVFS（240 / 160 / 80 MHz）+ FreeRTOS Tickless + 自动 light sleep。
 *
 * 工作方式：
 *  - Init() 调 esp_pm_configure(max=240, min=40, light_sleep_enable=true)。
 *    没有任何 PM lock 时，内核在 CPU 空闲时自动把主频降到 40 MHz，并进入 light sleep；
 *    有任务就绪或中断到来时立刻恢复，对应用层完全透明。
 *  - SetLevel() 由 Board::SetPowerSaveLevel 驱动，只切换"最高频率"：
 *    PERFORMANCE 240 MHz / BALANCED 160 MHz / LOW_POWER 80 MHz。
 *    min_freq 始终是 40 MHz，所以低负载时仍能降到 40 MHz。
 *  - NoLightSleepGuard / ApbMaxGuard 是 RAII 锁：音频编解码、SPI 屏幕传输、SD 卡读写
 *    这类对时序敏感的操作在作用域内禁止休眠 / 保证 APB 80 MHz。
 */
class PowerManager {
public:
    enum class Level {
        kPerformance,  // 240 MHz
        kBalanced,     // 160 MHz
        kLowPower,     // 80 MHz
    };

    static PowerManager& GetInstance();

    // 配置 esp_pm；未开启 CONFIG_PM_ENABLE 时安全返回 ESP_ERR_NOT_SUPPORTED
    esp_err_t Init();

    bool enabled() const { return enabled_; }
    Level level() const { return level_; }
    int max_freq_mhz() const { return max_freq_mhz_; }
    int min_freq_mhz() const { return kMinFreqMhz; }

    void SetLevel(Level level);

    // 禁止 light sleep（可嵌套，由 esp_pm 内部计数）
    void HoldNoLightSleep();
    void ReleaseNoLightSleep();
    // 保证 APB 80 MHz（SPI / I2S 传输）
    void HoldApbMax();
    void ReleaseApbMax();

    class NoLightSleepGuard {
    public:
        NoLightSleepGuard() { PowerManager::GetInstance().HoldNoLightSleep(); }
        ~NoLightSleepGuard() { PowerManager::GetInstance().ReleaseNoLightSleep(); }
        NoLightSleepGuard(const NoLightSleepGuard&) = delete;
        NoLightSleepGuard& operator=(const NoLightSleepGuard&) = delete;
    };

    class ApbMaxGuard {
    public:
        ApbMaxGuard() { PowerManager::GetInstance().HoldApbMax(); }
        ~ApbMaxGuard() { PowerManager::GetInstance().ReleaseApbMax(); }
        ApbMaxGuard(const ApbMaxGuard&) = delete;
        ApbMaxGuard& operator=(const ApbMaxGuard&) = delete;
    };

private:
    PowerManager() = default;
    PowerManager(const PowerManager&) = delete;
    PowerManager& operator=(const PowerManager&) = delete;

    esp_err_t Apply();

    static constexpr int kMinFreqMhz = 40;

    bool enabled_ = false;
    // 启动阶段先给最高频率（UI 初始化、字体加载更快）；应用层空闲时会调
    // SetPowerSaveLevel(LOW_POWER) 把上限降到 80 MHz。最低频率始终 40 MHz。
    Level level_ = Level::kPerformance;
    int max_freq_mhz_ = 240;
    esp_pm_lock_handle_t no_light_sleep_lock_ = nullptr;
    esp_pm_lock_handle_t apb_lock_ = nullptr;
};
