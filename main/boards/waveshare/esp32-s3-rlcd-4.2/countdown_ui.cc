// 独立倒计时页：黑底白字，只显示剩余时间（countdown_ui.cc）
//
// 设计约束（用户要求）：
//   - 页面只用于倒计时，不放置任何其它元素
//   - 收到倒计时指令自动切到本页
//   - 到点由 DataUpdateTask 调 Application::Alert 弹窗并播放 OGG_POPUP，
//     随后由倒计时状态机自动返回主页（或音乐页，若音乐仍在播放）
//
// 状态由 CustomLcdDisplay 持有，受 DisplayLock 保护；DataUpdateTask 每 500ms
// 调用 PollTransientUi() 递减，屏幕显示精确到秒。
#include "custom_lcd_display.h"
#include <cstdio>
#include <esp_log.h>
#include "application.h"
#include "ui_assets.h"

static const char *TAG = "CountdownUI";

void CustomLcdDisplay::SetupCountdownUI() {
    DisplayLockGuard lock(this);

    countdown_page_ = lv_obj_create(lv_screen_active());
    lv_obj_set_size(countdown_page_, UI_SCR_W, UI_SCR_H);
    lv_obj_set_pos(countdown_page_, 0, 0);
    lv_obj_set_style_bg_color(countdown_page_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(countdown_page_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(countdown_page_, 0, 0);
    lv_obj_set_style_pad_all(countdown_page_, 0, 0);
    lv_obj_set_style_radius(countdown_page_, 0, 0);
    lv_obj_remove_flag(countdown_page_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(countdown_page_, LV_OBJ_FLAG_CLICKABLE);

    // 唯一控件：剩余时间。alibaba_puhui_48 覆盖 0x20-0x7F（含 0-9 和冒号），1bpp。
    countdown_time_label_ = lv_label_create(countdown_page_);
    lv_obj_set_style_text_font(countdown_time_label_, &alibaba_puhui_48, 0);
    lv_obj_set_style_text_color(countdown_time_label_, lv_color_white(), 0);
    lv_obj_set_style_text_letter_space(countdown_time_label_, 4, 0);
    lv_label_set_text(countdown_time_label_, "00:00");
    lv_obj_center(countdown_time_label_);

    lv_obj_add_flag(countdown_page_, LV_OBJ_FLAG_HIDDEN);
    ESP_LOGI(TAG, "独立倒计时页创建完成（%dx%d，黑底白字）", UI_SCR_W, UI_SCR_H);
}

void CustomLcdDisplay::RenderCountdownLocked() {
    if (countdown_time_label_ == nullptr) {
        return;
    }
    if (countdown_count_ <= 0) {
        if (countdown_last_shown_sec_ != 0) {
            lv_label_set_text(countdown_time_label_, "00:00");
            countdown_last_shown_sec_ = 0;
        }
        return;
    }
    const uint32_t total_sec = (countdowns_[0].remaining_ms + 999u) / 1000u;
    const int shown = static_cast<int>(total_sec);
    if (shown == countdown_last_shown_sec_) {
        return;
    }
    countdown_last_shown_sec_ = shown;
    char buf[24];
    snprintf(buf, sizeof(buf), "%02lu:%02lu",
             static_cast<unsigned long>(total_sec / 60u),
             static_cast<unsigned long>(total_sec % 60u));
    lv_label_set_text(countdown_time_label_, buf);
}

bool CustomLcdDisplay::StartCountdown(int seconds) {
    if (seconds <= 0) {
        return false;
    }
    DisplayLockGuard lock(this);
    if (countdown_count_ >= kMaxCountdowns) {
        ESP_LOGW(TAG, "倒计时队列已满（%d）", kMaxCountdowns);
        return false;
    }
    countdowns_[countdown_count_].remaining_ms = static_cast<uint32_t>(seconds) * 1000u;
    countdown_count_++;
    countdown_active_.store(true);
    countdown_paused_ = false;
    countdown_last_tick_ms_ = lv_tick_get();
    countdown_last_shown_sec_ = -1;
    display_mode_ = MODE_COUNTDOWN;
    ApplyDisplayMode();
    RenderCountdownLocked();
    ESP_LOGI(TAG, "倒计时开始: %d 秒（队列 %d 个）", seconds, countdown_count_);
    return true;
}

bool CustomLcdDisplay::PauseCountdown() {
    DisplayLockGuard lock(this);
    if (countdown_count_ <= 0) {
        return false;
    }
    countdown_paused_ = true;
    ESP_LOGI(TAG, "倒计时已暂停");
    return true;
}

bool CustomLcdDisplay::ResumeCountdown() {
    DisplayLockGuard lock(this);
    if (countdown_count_ <= 0) {
        return false;
    }
    countdown_paused_ = false;
    countdown_last_tick_ms_ = lv_tick_get();
    ESP_LOGI(TAG, "倒计时已继续");
    return true;
}

bool CustomLcdDisplay::CancelCountdown() {
    DisplayLockGuard lock(this);
    if (countdown_count_ <= 0) {
        return false;
    }
    countdown_count_ = 0;
    countdown_active_.store(false);
    countdown_paused_ = false;
    countdown_last_shown_sec_ = -1;
    if (countdown_time_label_) {
        lv_label_set_text(countdown_time_label_, "00:00");
    }
    display_mode_ = Application::GetInstance().IsMusicPlaying() ? MODE_MUSIC : MODE_WEATHER;
    ApplyDisplayMode();
    ESP_LOGI(TAG, "倒计时已取消");
    return true;
}

int CustomLcdDisplay::GetCountdownRemainingSec() const {
    DisplayLockGuard lock(const_cast<CustomLcdDisplay*>(this));
    if (countdown_count_ <= 0) {
        return 0;
    }
    return static_cast<int>((countdowns_[0].remaining_ms + 999u) / 1000u);
}

void CustomLcdDisplay::ShowWeatherDetailForQuery(uint32_t auto_return_ms) {
    DisplayLockGuard lock(this);
    // 倒计时页优先级最高；音乐播放期间保持音乐页（避免打断正在播放的体验）
    if (countdown_active_.load()) {
        ESP_LOGI(TAG, "倒计时进行中，天气查询仅播报，不切页");
        return;
    }
    if (Application::GetInstance().IsMusicPlaying()) {
        ESP_LOGI(TAG, "音乐播放中，天气查询仅播报，不切页");
        return;
    }
    detail_return_deadline_ms_ = lv_tick_get() + auto_return_ms;
    detail_wait_speaking_ = true;
    detail_saw_speaking_ = false;
    display_mode_ = MODE_DETAIL;
    ApplyDisplayMode();
    ESP_LOGI(TAG, "天气查询：切详情页，%u ms 后自动返回", static_cast<unsigned>(auto_return_ms));
}

void CustomLcdDisplay::MarkCountdownAlert(uint32_t now_ms) {
    countdown_alert_clear_ms_.store(now_ms + 4000u);
}

bool CustomLcdDisplay::ShouldDismissCountdownAlert(uint32_t now_ms) {
    uint32_t due = countdown_alert_clear_ms_.load();
    if (due != 0 && static_cast<int32_t>(now_ms - due) >= 0) {
        countdown_alert_clear_ms_.store(0);
        return true;
    }
    return false;
}

int CustomLcdDisplay::PollTransientUi(uint32_t now_ms) {
    int expired = 0;
    DisplayLockGuard lock(this);

    if (countdown_count_ > 0) {
        uint32_t delta = now_ms - countdown_last_tick_ms_;
        countdown_last_tick_ms_ = now_ms;
        if (delta > 10000u) {
            delta = 10000u;  // 防止任务被长时间调度后一次性扣太多
        }
        if (!countdown_paused_ && delta > 0) {
            for (int i = 0; i < countdown_count_; ++i) {
                if (countdowns_[i].remaining_ms <= delta) {
                    countdowns_[i].remaining_ms = 0;
                } else {
                    countdowns_[i].remaining_ms -= delta;
                }
            }
        }
        // 移除已到点的项（多个同时到点则一次提醒）。保留队列顺序，依次显示下一个。
        int w = 0;
        for (int i = 0; i < countdown_count_; ++i) {
            if (countdowns_[i].remaining_ms == 0) {
                expired++;
            } else {
                countdowns_[w++] = countdowns_[i];
            }
        }
        countdown_count_ = w;
        if (countdown_count_ == 0) {
            countdown_active_.store(false);
            countdown_paused_ = false;
            display_mode_ = Application::GetInstance().IsMusicPlaying() ? MODE_MUSIC : MODE_WEATHER;
            ApplyDisplayMode();
        } else {
            RenderCountdownLocked();
        }
    }

    if (detail_return_deadline_ms_ != 0) {
        DeviceState ds = Application::GetInstance().GetDeviceState();
        if (ds == kDeviceStateSpeaking) {
            detail_saw_speaking_ = true;
        }
        const int32_t left_ms = static_cast<int32_t>(detail_return_deadline_ms_ - now_ms);
        const bool fallback_due = (left_ms <= 0);
        const bool speech_done = detail_wait_speaking_ && detail_saw_speaking_ && ds == kDeviceStateIdle;
        if (fallback_due || speech_done) {
            detail_return_deadline_ms_ = 0;
            detail_wait_speaking_ = false;
            detail_saw_speaking_ = false;
            if (countdown_count_ == 0) {
                display_mode_ = Application::GetInstance().IsMusicPlaying() ? MODE_MUSIC : MODE_WEATHER;
                ApplyDisplayMode();
                ESP_LOGI(TAG, "天气播报结束，自动返回%s", display_mode_ == MODE_MUSIC ? "音乐页" : "主页");
            }
        }
    }

    return expired;
}
