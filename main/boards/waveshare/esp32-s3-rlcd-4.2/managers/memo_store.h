#pragma once

#include <mutex>

// 手动备忘与待办已拆分到不同 NVS key；用一个进程内共享互斥量保护
// 各自“读-改-写”全过程，避免跨任务互相覆盖。
constexpr const char* kMemoNvsNamespace = "memo";
constexpr const char* kMemoManualKey = "manual";   // 手动备忘
constexpr const char* kMemoTodoKey = "todos";      // MCP/服务端待办
constexpr const char* kMemoLegacyKey = "items";    // 旧版混存 key，仅用于一次性迁移

inline std::mutex& MemoStoreMutex() {
    static std::mutex mutex;
    return mutex;
}
