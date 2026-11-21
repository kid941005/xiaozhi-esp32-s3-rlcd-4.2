#ifndef TODO_MCP_CLIENT_H
#define TODO_MCP_CLIENT_H

#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

class WebSocket;

// 设备侧 MCP 客户端：连接外部 MCP 接入点（websocket），保持长连接，
// 周期性调用 list_todos 拉取当日待办，写入 NVS 并刷新右侧待办卡。
//
// 之所以保持长连接而不是每次新建：每次新建都要重新做一次 TLS 握手，
// 实测周期性重连会偶发握手失败（对端在握手阶段关闭 TCP），长连接只需要
// 在断线后重连，且刷新更快（10 秒）。
class TodoMcpClient {
public:
    static TodoMcpClient& GetInstance();

    // 启动后台任务。on_updated 在成功刷新待办后调用（用于刷新屏幕）
    void Start(std::function<void()> on_updated);

    // 立即触发一次拉取（例如网络刚连上时）
    void RequestRefresh();

    // 接入点地址：持久化到 NVS（namespace "todo", key "mcp_url"）
    void SetUrl(const std::string& url);
    std::string GetUrl();

    // 最近一次 music_status 的元数据（飞牛音乐 MCP）。音乐页空闲时用于显示上次播放。
    std::string GetLastMusicTitle();
    std::string GetLastMusicArtist();
    bool ConsumeMusicUpdated();

private:
    TodoMcpClient() = default;
    ~TodoMcpClient() = default;
    TodoMcpClient(const TodoMcpClient&) = delete;
    TodoMcpClient& operator=(const TodoMcpClient&) = delete;

    void Run();
    // 建立连接并完成 initialize 握手，失败会内部重试
    bool OpenSession(std::unique_ptr<WebSocket>& ws, const std::string& url);
    // 一条长连接内的全部拉取，返回时连接已不可用
    void RunSession(const std::string& url);
    bool ApplyPayload(const std::string& payload);
    bool ApplyReminderApiPayload(const std::string& payload);
    bool ApplyMusicStatusPayload(const std::string& payload);
    static void TaskEntry(void* arg);
    static void HttpTaskEntry(void* arg);
    void RunHttp();
    bool FetchTodosDirect();

    EventGroupHandle_t event_group_ = nullptr;
    std::function<void()> on_updated_;
    std::mutex mutex_;
    std::mutex nvs_mutex_;
    std::string msg_reply_;
    std::string music_title_;
    std::string music_artist_;
    bool music_updated_ = false;
    int pending_id_ = 0;
    int last_list_error_code_ = 0;
    // 外部 MCP 明确返回 -32601（工具不存在）且 REST 直连可用时进入长退避，
    // 避免每 5 秒重连一个永远不会成功的接入点。
    bool mcp_tool_missing_ = false;
    volatile bool broken_ = false;
};

#endif  // TODO_MCP_CLIENT_H
