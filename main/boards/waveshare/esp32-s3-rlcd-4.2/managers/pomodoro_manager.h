#ifndef __POMODORO_MANAGER_H__
#define __POMODORO_MANAGER_H__

#include <atomic>
#include <string>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// 番茄钟管理器（单例）
//
// 功能：
//   1. 纯倒计时（用户指定分钟数，倒计时到 0 结束）
//   2. 倒计时结束后屏幕提醒
//
// 入口：通过 AI 语音调用 MCP 工具 self.pomodoro.start / stop / status
class PomodoroManager {
public:
    // 番茄钟状态（简化版：只有空闲、倒计时中、已暂停）
    enum State {
        IDLE = 0,       // 空闲（未启动）
        COUNTING,       // 倒计时中
        PAUSED,         // 已暂停
    };

    static PomodoroManager& getInstance() {
        static PomodoroManager instance;
        return instance;
    }

    // 开始倒计时（minutes: 倒计时分钟数）
    bool start(int minutes = 25);

    // 停止倒计时
    void stop();

    // 暂停/恢复
    void togglePause();

    // 获取当前状态
    State getState() const { return state_.load(); }

    // 获取剩余秒数
    int getRemainingSeconds() const { return remaining_seconds_.load(); }

    // 获取总秒数
    int getTotalSeconds() const { return total_seconds_.load(); }

    // 获取设定的分钟数
    int getMinutes() const { return minutes_; }

    // 获取状态文字描述
    std::string getStateText() const;

    // 获取格式化的剩余时间 "MM:SS"
    std::string getRemainingTimeStr() const;

private:
    PomodoroManager() = default;
    ~PomodoroManager();

    PomodoroManager(const PomodoroManager&) = delete;
    PomodoroManager& operator=(const PomodoroManager&) = delete;

    // 倒计时主任务
    static void PomodoroTask(void* arg);

    static constexpr const char* TAG = "PomodoroManager";

    std::atomic<State> state_{IDLE};
    std::atomic<int> remaining_seconds_{0};
    std::atomic<int> total_seconds_{0};
    std::atomic<bool> stop_requested_{false};

    int minutes_ = 25;

    TaskHandle_t pomodoro_task_handle_ = nullptr;
};

#endif
