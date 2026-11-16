#ifndef TODO_MCP_CLIENT_H
#define TODO_MCP_CLIENT_H

#include <functional>
#include <mutex>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

// 设备侧 MCP 客户端：周期性连接外部 MCP 接入点（websocket），
// 调用 list_todos 拉取当日待办，写入 NVS 并刷新右侧待办卡。
class TodoMcpClient {
public:
    static TodoMcpClient& GetInstance();

    // 启动后台拉取任务。on_updated 在成功刷新待办后调用（用于刷新屏幕）
    void Start(std::function<void()> on_updated);

    // 立即触发一次拉取（例如网络刚连上时）
    void RequestRefresh();

    // 接入点地址：持久化到 NVS（namespace "todo", key "mcp_url"）
    void SetUrl(const std::string& url);
    std::string GetUrl();

private:
    TodoMcpClient() = default;
    ~TodoMcpClient() = default;
    TodoMcpClient(const TodoMcpClient&) = delete;
    TodoMcpClient& operator=(const TodoMcpClient&) = delete;

    void Run();
    bool PullOnce();
    bool ApplyPayload(const std::string& payload);
    static void TaskEntry(void* arg);

    EventGroupHandle_t event_group_ = nullptr;
    std::function<void()> on_updated_;
    std::mutex mutex_;
    std::string msg_init_;
    std::string msg_tools_;
};

#endif  // TODO_MCP_CLIENT_H
