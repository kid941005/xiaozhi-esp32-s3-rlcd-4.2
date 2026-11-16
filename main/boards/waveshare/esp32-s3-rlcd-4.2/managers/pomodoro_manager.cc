#include "pomodoro_manager.h"
#include <esp_log.h>
#include "board.h"
#include "display/display.h"
#include <cstdio>

// ===== 公开接口 =====

bool PomodoroManager::start(int minutes) {
    // 如果已经在运行，先停止
    if (state_.load() != IDLE) {
        stop();
        vTaskDelay(pdMS_TO_TICKS(500));  // 等待旧任务清理
    }

    minutes_ = minutes;

    int total_secs = minutes * 60;
    stop_requested_.store(false);
    remaining_seconds_.store(total_secs);
    total_seconds_.store(total_secs);
    state_.store(COUNTING);

    // 启动倒计时主任务
    xTaskCreatePinnedToCore(PomodoroTask, "pomodoro_task", 4 * 1024, this, 2, &pomodoro_task_handle_, 0);

    ESP_LOGI(TAG, "番茄钟已启动: %d 分钟倒计时", minutes);
    return true;
}

void PomodoroManager::stop() {
    if (state_.load() == IDLE) return;

    stop_requested_.store(true);
    state_.store(IDLE);
    remaining_seconds_.store(0);
    total_seconds_.store(0);

    // 等待任务退出
    if (pomodoro_task_handle_) {
        int wait_count = 0;
        while (pomodoro_task_handle_ && wait_count < 20) {
            vTaskDelay(pdMS_TO_TICKS(100));
            wait_count++;
        }
    }

    ESP_LOGI(TAG, "番茄钟已停止");
}

void PomodoroManager::togglePause() {
    State current = state_.load();
    if (current == COUNTING) {
        state_.store(PAUSED);
        ESP_LOGI(TAG, "番茄钟已暂停");
    } else if (current == PAUSED) {
        state_.store(COUNTING);
        ESP_LOGI(TAG, "番茄钟已恢复");
    }
}

std::string PomodoroManager::getStateText() const {
    switch (state_.load()) {
        case IDLE:     return "空闲";
        case COUNTING: return "倒计时中";
        case PAUSED:   return "已暂停";
        default:       return "未知";
    }
}

std::string PomodoroManager::getRemainingTimeStr() const {
    int secs = remaining_seconds_.load();
    int min = secs / 60;
    int sec = secs % 60;
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d", min, sec);
    return std::string(buf);
}

PomodoroManager::~PomodoroManager() {
    stop();
}

// ===== 倒计时主任务（纯倒计时，到 0 就结束） =====

void PomodoroManager::PomodoroTask(void* arg) {
    auto* self = static_cast<PomodoroManager*>(arg);

    ESP_LOGI(TAG, "倒计时任务启动，共 %d 秒", self->remaining_seconds_.load());

    while (!self->stop_requested_.load()) {
        State current = self->state_.load();

        if (current == PAUSED) {
            // 暂停状态，不倒计时
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        if (current == IDLE) {
            break;
        }

        // 每秒倒计时
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (self->stop_requested_.load()) break;
        if (self->state_.load() == PAUSED) continue;

        int remaining = self->remaining_seconds_.load();
        if (remaining > 0) {
            self->remaining_seconds_.store(remaining - 1);
        }

        // 倒计时结束
        if (remaining <= 1) {
            ESP_LOGI(TAG, "倒计时结束！");

            // 屏幕显示提醒
            auto display = Board::GetInstance().GetDisplay();
            if (display) {
                display->SetChatMessage("system", "时间到！倒计时结束~");
            }

            // 回到空闲
            self->state_.store(IDLE);
            self->remaining_seconds_.store(0);
            self->total_seconds_.store(0);
            break;
        }
    }

    self->pomodoro_task_handle_ = nullptr;
    ESP_LOGI(TAG, "倒计时任务退出");
    vTaskDelete(nullptr);
}
