#include "power_manager.h"

#include <esp_log.h>

static const char* TAG = "PowerManager";

PowerManager& PowerManager::GetInstance() {
    static PowerManager instance;
    return instance;
}

esp_err_t PowerManager::Init() {
    if (enabled_) {
        return ESP_OK;
    }

    esp_err_t ret = esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "rlcd_no_sleep", &no_light_sleep_lock_);
    if (ret != ESP_OK) {
        // CONFIG_PM_ENABLE 未开启时返回 ESP_ERR_NOT_SUPPORTED，其余情况是真正的错误
        if (ret != ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGE(TAG, "创建 NO_LIGHT_SLEEP 锁失败: %s", esp_err_to_name(ret));
        }
        no_light_sleep_lock_ = nullptr;
        apb_lock_ = nullptr;
        return ret;
    }

    ret = esp_pm_lock_create(ESP_PM_APB_FREQ_MAX, 0, "rlcd_apb", &apb_lock_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "创建 APB_FREQ_MAX 锁失败: %s", esp_err_to_name(ret));
        apb_lock_ = nullptr;
        esp_pm_lock_delete(no_light_sleep_lock_);
        no_light_sleep_lock_ = nullptr;
        return ret;
    }

    enabled_ = true;
    ret = Apply();
    if (ret != ESP_OK) {
        enabled_ = false;
        return ret;
    }
    return ESP_OK;
}

esp_err_t PowerManager::Apply() {
    esp_pm_config_t config = {};
    config.max_freq_mhz = max_freq_mhz_;
    config.min_freq_mhz = kMinFreqMhz;
    config.light_sleep_enable = true;

    esp_err_t ret = esp_pm_configure(&config);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "esp_pm_configure 失败: %s（max=%d min=%d）", esp_err_to_name(ret),
                 config.max_freq_mhz, config.min_freq_mhz);
        return ret;
    }
    ESP_LOGI(TAG, "DVFS 已启用: CPU %d~%d MHz, 自动 light sleep 开启", kMinFreqMhz, max_freq_mhz_);
    return ESP_OK;
}

void PowerManager::SetLevel(Level level) {
    if (level_ == level) {
        return;
    }
    level_ = level;

    int target = max_freq_mhz_;
    switch (level) {
        case Level::kPerformance:
            target = 240;
            break;
        case Level::kBalanced:
            target = 160;
            break;
        case Level::kLowPower:
        default:
            target = 80;
            break;
    }

    if (target == max_freq_mhz_) {
        return;
    }
    max_freq_mhz_ = target;
    if (enabled_) {
        Apply();
    }
}

void PowerManager::HoldNoLightSleep() {
    if (no_light_sleep_lock_ != nullptr) {
        esp_pm_lock_acquire(no_light_sleep_lock_);
    }
}

void PowerManager::ReleaseNoLightSleep() {
    if (no_light_sleep_lock_ != nullptr) {
        esp_pm_lock_release(no_light_sleep_lock_);
    }
}

void PowerManager::HoldApbMax() {
    if (apb_lock_ != nullptr) {
        esp_pm_lock_acquire(apb_lock_);
    }
}

void PowerManager::ReleaseApbMax() {
    if (apb_lock_ != nullptr) {
        esp_pm_lock_release(apb_lock_);
    }
}
