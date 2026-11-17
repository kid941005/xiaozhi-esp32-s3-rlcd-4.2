#include "todo_mcp_client.h"

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_system.h>

#include <cstdio>
#include <cstring>
#include <ctime>

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
constexpr EventBits_t kReplyBit = BIT2;

constexpr int kInitTimeoutMs = 10000;
constexpr int kCallTimeoutMs = 20000;
constexpr int kPollMs = 60 * 1000;      // 长连接下的拉取间隔
constexpr int kRetryMs = 20 * 1000;     // 断线后重连间隔
constexpr int kFirstDelayMs = 20000;    // 上电后先等网络
constexpr int kConnectAttempts = 3;     // 建连失败重试次数
constexpr int kConnectGapMs = 3000;
constexpr int kMaxConsecFail = 2;       // 连续拉取失败多少次后重连
constexpr int kMaxRows = 10;

// 0=周日 … 6=周六
int DayOfWeek(int y, int m, int d) {
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) y -= 1;
    int w = (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
    return (w + 7) % 7;
}

// 只保留今天的待办：服务端已用 view=today 过滤，这里按本地日期再兜一次底
bool IsToday(const char* due_text) {
    if (due_text == nullptr || std::strlen(due_text) < 10) {
        return true;   // 没有日期字段时视为今天
    }
    int y = 0, mo = 0, d = 0;
    if (std::sscanf(due_text, "%d-%d-%d", &y, &mo, &d) != 3) {
        return true;
    }
    time_t now = time(nullptr);
    struct tm tm_now = {};
    localtime_r(&now, &tm_now);
    return y == (tm_now.tm_year + 1900) && mo == (tm_now.tm_mon + 1) && d == tm_now.tm_mday;
}

// 时间行 = 「HH:MM」；全天待办给「全天」，无时间给空串（UI 显示占位）
std::string FormatTime(const char* due_text, bool all_day) {
    if (all_day) return std::string("全天");
    if (due_text == nullptr || std::strlen(due_text) < 16) return std::string();
    int hh = 0, mm = 0;
    if (std::sscanf(due_text + 11, "%d:%d", &hh, &mm) != 2) return std::string();
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", hh, mm);
    return std::string(buf);
}

const char* kInitRequest =
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{"
    "\"protocolVersion\":\"2024-11-05\",\"capabilities\":{},"
    "\"clientInfo\":{\"name\":\"esp32-rlcd-4.2\",\"version\":\"2.6.0\"}}}";

const char* kInitializedNote =
    "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}";

std::string BuildListTodosRequest(int id) {
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"tools/call\",\"params\":"
                  "{\"name\":\"list_todos\",\"arguments\":{\"view\":\"today\",\"limit\":10}}}",
                  id);
    return std::string(buf);
}

void LogHeap(const char* when) {
    ESP_LOGI(TAG, "%s：空闲内存 %d 字节，最大连续块 %d 字节", when,
             static_cast<int>(esp_get_free_heap_size()),
             static_cast<int>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
}
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
        const std::string url = GetUrl();
        if (!url.empty()) {
            RunSession(url);
        }
        // 会话结束（断线/未配置），等一会儿或等网络事件再重连
        xEventGroupClearBits(event_group_, kRefreshBit);
        xEventGroupWaitBits(event_group_, kRefreshBit, pdTRUE, pdFALSE, pdMS_TO_TICKS(kRetryMs));
    }
}

bool TodoMcpClient::OpenSession(std::unique_ptr<WebSocket>& ws, const std::string& url) {
    NetworkInterface* network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        ESP_LOGW(TAG, "网络未就绪");
        return false;
    }

    for (int attempt = 1; attempt <= kConnectAttempts; ++attempt) {
        broken_ = false;
        ws = network->CreateWebSocket(2);
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
                const int got = id->valueint;
                if (got == 1) {
                    xEventGroupSetBits(event_group_, kInitBit);
                } else if (got == pending_id_) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    msg_reply_.assign(data, len);
                    xEventGroupSetBits(event_group_, kReplyBit);
                }
            }
            cJSON_Delete(root);
        });
        ws->OnError([this](const NetworkError& error) {
            ESP_LOGW(TAG, "websocket 错误: %s", error.ToString().c_str());
            broken_ = true;
        });

        xEventGroupClearBits(event_group_, kInitBit | kReplyBit);
        LogHeap("建连前");
        ESP_LOGI(TAG, "连接待办 MCP 接入点(%d/%d): %s", attempt, kConnectAttempts, url.c_str());
        if (auto connected = ws->Connect(url.c_str()); connected) {
            return true;
        } else {
            ESP_LOGW(TAG, "连接失败: %s", connected.error().ToString().c_str());
        }
        ws->Close();
        ws.reset();
        if (attempt < kConnectAttempts) {
            vTaskDelay(pdMS_TO_TICKS(kConnectGapMs));
        }
    }
    return false;
}

void TodoMcpClient::RunSession(const std::string& url) {
    std::unique_ptr<WebSocket> ws;
    if (!OpenSession(ws, url)) {
        return;
    }

    if (!ws->Send(kInitRequest)) {
        ESP_LOGW(TAG, "发送 initialize 失败");
        ws->Close();
        return;
    }
    EventBits_t bits = xEventGroupWaitBits(event_group_, kInitBit, pdTRUE, pdFALSE,
                                           pdMS_TO_TICKS(kInitTimeoutMs));
    if (!(bits & kInitBit)) {
        ESP_LOGW(TAG, "等待 initialize 响应超时");
        ws->Close();
        return;
    }
    ws->Send(kInitializedNote);
    ESP_LOGI(TAG, "MCP 会话已建立");

    int consec_fail = 0;
    for (;;) {
        if (broken_) {
            ESP_LOGW(TAG, "连接已断开");
            break;
        }
        pending_id_ = 2;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            msg_reply_.clear();
        }
        xEventGroupClearBits(event_group_, kReplyBit);

        const std::string request = BuildListTodosRequest(pending_id_);
        if (!ws->Send(request)) {
            ESP_LOGW(TAG, "发送 tools/call 失败");
            break;
        }
        bits = xEventGroupWaitBits(event_group_, kReplyBit, pdTRUE, pdFALSE,
                                   pdMS_TO_TICKS(kCallTimeoutMs));
        if (bits & kReplyBit) {
            std::string payload;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                payload = msg_reply_;
            }
            if (ApplyPayload(payload)) {
                consec_fail = 0;
            } else {
                ++consec_fail;
            }
        } else {
            ++consec_fail;
            ESP_LOGW(TAG, "等待 list_todos 响应超时（连续 %d 次）", consec_fail);
        }
        if (consec_fail >= kMaxConsecFail) {
            ESP_LOGW(TAG, "连续 %d 次拉取失败，重建连接", consec_fail);
            break;
        }

        // 等到下一次拉取，期间网络恢复事件可以提前触发
        xEventGroupClearBits(event_group_, kRefreshBit);
        xEventGroupWaitBits(event_group_, kRefreshBit, pdTRUE, pdFALSE, pdMS_TO_TICKS(kPollMs));
    }

    ws->Close();
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
                    if (!IsToday(due_text)) {
                        continue;   // 非今天的待办不上屏
                    }
                    std::string tstr = FormatTime(due_text, cJSON_IsTrue(all_day));
                    ESP_LOGI(TAG, "待办行: %s %s", tstr.c_str(), title->valuestring);

                    cJSON* item = cJSON_CreateObject();
                    cJSON_AddStringToObject(item, "t", tstr.c_str());
                    cJSON_AddStringToObject(item, "c", title->valuestring);
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
