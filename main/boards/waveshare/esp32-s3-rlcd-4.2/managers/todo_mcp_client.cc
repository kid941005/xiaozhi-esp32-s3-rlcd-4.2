#include "todo_mcp_client.h"

#include <cJSON.h>
#include <esp_log.h>

#include <cstdio>
#include <cstring>
#include <memory>

#include <board.h>
#include <network_interface.h>
#include <settings.h>
#include <web_socket.h>

#include "../config.h"

// 本地私有配置（含 token，已在 .gitignore 中排除），存在时覆盖 config.h 的默认值
#if __has_include("todo_mcp_secret.h")
#include "todo_mcp_secret.h"
#endif

#define TAG "TodoMcp"

#ifndef MCP_TODO_URL
#define MCP_TODO_URL ""
#endif

namespace {
constexpr EventBits_t kRefreshBit = BIT0;
constexpr EventBits_t kInitBit = BIT1;
constexpr EventBits_t kToolsBit = BIT2;

constexpr int kConnectTimeoutMs = 15000;
constexpr int kInitTimeoutMs = 10000;
constexpr int kToolsTimeoutMs = 20000;
constexpr int kPollOkMs = 5 * 60 * 1000;   // 成功：5 分钟
constexpr int kPollFailMs = 30 * 1000;     // 失败：30 秒重试
constexpr int kFirstDelayMs = 20000;       // 上电后先等网络
constexpr int kMaxRows = 10;

// 0=周日 … 6=周六
int DayOfWeek(int y, int m, int d) {
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) y -= 1;
    int w = (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
    return (w + 7) % 7;
}

// "2026-11-16 20:00" -> "11.16(日) 20:00 "
std::string FormatRow(const char* title, const char* due_text, bool all_day) {
    std::string out;
    int y = 0, mo = 0, d = 0;
    if (due_text != nullptr && std::strlen(due_text) >= 10 &&
        std::sscanf(due_text, "%d-%d-%d", &y, &mo, &d) == 3 && mo >= 1 && mo <= 12) {
        static const char* kWeek[] = {"日", "一", "二", "三", "四", "五", "六"};
        const int w = DayOfWeek(y, mo, d);
        char buf[40];
        int hh = 0, mm = 0;
        if (!all_day && std::strlen(due_text) >= 16 &&
            std::sscanf(due_text + 11, "%d:%d", &hh, &mm) == 2) {
            std::snprintf(buf, sizeof(buf), "%02d.%02d(%s) %02d:%02d ", mo, d, kWeek[w], hh, mm);
        } else {
            std::snprintf(buf, sizeof(buf), "%02d.%02d(%s) ", mo, d, kWeek[w]);
        }
        out = buf;
    }
    if (title != nullptr) out += title;
    return out;
}

const char* kInitRequest =
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{"
    "\"protocolVersion\":\"2024-11-05\",\"capabilities\":{},"
    "\"clientInfo\":{\"name\":\"esp32-rlcd-4.2\",\"version\":\"2.6.0\"}}}";

const char* kInitializedNote =
    "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}";

const char* kListTodosRequest =
    "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{"
    "\"name\":\"list_todos\",\"arguments\":{\"view\":\"today\",\"limit\":10}}}";
}  // namespace

TodoMcpClient& TodoMcpClient::GetInstance() {
    static TodoMcpClient instance;
    return instance;
}

void TodoMcpClient::SetUrl(const std::string& url) {
    Settings settings("todo", true);
    settings.SetString("mcp_url", url);
}

std::string TodoMcpClient::GetUrl() {
    Settings settings("todo", false);
    std::string url = settings.GetString("mcp_url", "");
    if (url.empty() && std::strlen(MCP_TODO_URL) > 0) {
        url = MCP_TODO_URL;
        SetUrl(url);
    }
    return url;
}

void TodoMcpClient::RequestRefresh() {
    if (event_group_ != nullptr) {
        xEventGroupSetBits(event_group_, kRefreshBit);
    }
}

void TodoMcpClient::TaskEntry(void* arg) {
    static_cast<TodoMcpClient*>(arg)->Run();
}

void TodoMcpClient::Start(std::function<void()> on_updated) {
    if (event_group_ != nullptr) return;
    on_updated_ = std::move(on_updated);
    event_group_ = xEventGroupCreate();
    if (event_group_ == nullptr) {
        ESP_LOGE(TAG, "创建事件组失败");
        return;
    }
    // 首次启动时把编译期默认地址落盘，之后以 NVS 为准
    GetUrl();
    xTaskCreate(TaskEntry, "todo_mcp", 8192, this, 2, nullptr);
}

void TodoMcpClient::Run() {
    vTaskDelay(pdMS_TO_TICKS(kFirstDelayMs));
    for (;;) {
        const bool ok = PullOnce();
        // 拉取期间网络事件可能已经置位，先清掉再等，避免紧接着再连一次
        // （服务端对同一 token 的并发/快速重连会 TLS 握手失败）
        xEventGroupClearBits(event_group_, kRefreshBit);
        xEventGroupWaitBits(event_group_, kRefreshBit, pdTRUE, pdFALSE,
                            pdMS_TO_TICKS(ok ? kPollOkMs : kPollFailMs));
    }
}

bool TodoMcpClient::PullOnce() {
    const std::string url = GetUrl();
    if (url.empty()) {
        // 未配置接入点：安静地待机
        return true;
    }

    NetworkInterface* network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        return false;
    }

    auto ws = network->CreateWebSocket(2);
    if (ws == nullptr) {
        ESP_LOGE(TAG, "创建 websocket 失败");
        return false;
    }

    ws->SetReceiveBufferSize(8192);
    ws->OnData([this](const char* data, size_t len, bool binary) {
        if (binary || data == nullptr) return;
        cJSON* root = cJSON_ParseWithLength(data, len);
        if (root == nullptr) return;
        cJSON* id = cJSON_GetObjectItem(root, "id");
        if (cJSON_IsNumber(id)) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (id->valueint == 1) {
                msg_init_.assign(data, len);
                xEventGroupSetBits(event_group_, kInitBit);
            } else if (id->valueint == 2) {
                msg_tools_.assign(data, len);
                xEventGroupSetBits(event_group_, kToolsBit);
            }
        }
        cJSON_Delete(root);
    });
    ws->OnError([](const NetworkError& error) {
        ESP_LOGW(TAG, "websocket 错误: %s", error.ToString().c_str());
    });

    {
        std::lock_guard<std::mutex> lock(mutex_);
        msg_init_.clear();
        msg_tools_.clear();
    }
    xEventGroupClearBits(event_group_, kInitBit | kToolsBit);

    ESP_LOGI(TAG, "连接待办 MCP 接入点: %s", url.c_str());
    if (auto connected = ws->Connect(url.c_str()); !connected) {
        ESP_LOGW(TAG, "连接失败: %s", connected.error().ToString().c_str());
        return false;
    }

    if (!ws->Send(kInitRequest)) {
        ESP_LOGW(TAG, "发送 initialize 失败");
        ws->Close();
        return false;
    }
    EventBits_t bits = xEventGroupWaitBits(event_group_, kInitBit, pdTRUE, pdFALSE,
                                           pdMS_TO_TICKS(kInitTimeoutMs));
    if (!(bits & kInitBit)) {
        ESP_LOGW(TAG, "等待 initialize 响应超时");
        ws->Close();
        return false;
    }

    ws->Send(kInitializedNote);
    if (!ws->Send(kListTodosRequest)) {
        ESP_LOGW(TAG, "发送 tools/call 失败");
        ws->Close();
        return false;
    }
    bits = xEventGroupWaitBits(event_group_, kToolsBit, pdTRUE, pdFALSE,
                               pdMS_TO_TICKS(kToolsTimeoutMs));
    if (!(bits & kToolsBit)) {
        ESP_LOGW(TAG, "等待 list_todos 响应超时");
        ws->Close();
        return false;
    }

    std::string payload;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        payload = msg_tools_;
    }
    ws->Close();
    return ApplyPayload(payload);
}

bool TodoMcpClient::ApplyPayload(const std::string& payload) {
    if (payload.empty()) return false;

    cJSON* root = cJSON_Parse(payload.c_str());
    if (root == nullptr) return false;

    bool ok = false;
    int count = 0;
    cJSON* arr = cJSON_CreateArray();

    cJSON* result = cJSON_GetObjectItem(root, "result");
    cJSON* content = (result != nullptr) ? cJSON_GetObjectItem(result, "content") : nullptr;
    cJSON* first = (content != nullptr && cJSON_IsArray(content)) ? cJSON_GetArrayItem(content, 0) : nullptr;
    cJSON* text = (first != nullptr) ? cJSON_GetObjectItem(first, "text") : nullptr;

    if (text != nullptr && cJSON_IsString(text) && text->valuestring != nullptr) {
        cJSON* inner = cJSON_Parse(text->valuestring);
        if (inner != nullptr) {
            cJSON* ok_item = cJSON_GetObjectItem(inner, "ok");
            ok = cJSON_IsBool(ok_item) ? cJSON_IsTrue(ok_item) : true;
            cJSON* items = cJSON_GetObjectItem(inner, "items");
            if (ok && items != nullptr && cJSON_IsArray(items)) {
                cJSON* it = nullptr;
                cJSON_ArrayForEach(it, items) {
                    if (count >= kMaxRows) break;
                    cJSON* title = cJSON_GetObjectItem(it, "title");
                    if (title == nullptr || !cJSON_IsString(title) || title->valuestring == nullptr ||
                        title->valuestring[0] == 0) {
                        continue;
                    }
                    cJSON* due = cJSON_GetObjectItem(it, "due_text");
                    cJSON* all_day = cJSON_GetObjectItem(it, "all_day");
                    const char* due_text =
                        (due != nullptr && cJSON_IsString(due) && due->valuestring != nullptr)
                            ? due->valuestring
                            : "";
                    std::string row = FormatRow(title->valuestring, due_text, cJSON_IsTrue(all_day));
                    ESP_LOGI(TAG, "待办行: %s", row.c_str());

                    cJSON* item = cJSON_CreateObject();
                    cJSON_AddStringToObject(item, "t", "");
                    cJSON_AddStringToObject(item, "c", row.c_str());
                    cJSON_AddItemToArray(arr, item);
                    count++;
                }
            }
            cJSON_Delete(inner);
        }
    }
    cJSON_Delete(root);

    if (!ok) {
        cJSON_Delete(arr);
        ESP_LOGW(TAG, "待办服务返回失败，保留原列表");
        return false;
    }

    char* json = cJSON_PrintUnformatted(arr);
    if (json != nullptr) {
        Settings settings("memo", true);
        settings.SetString("items", json);
        cJSON_free(json);
    }
    cJSON_Delete(arr);

    ESP_LOGI(TAG, "已拉取当日待办 %d 条", count);
    if (on_updated_) {
        on_updated_();
    }
    return true;
}
