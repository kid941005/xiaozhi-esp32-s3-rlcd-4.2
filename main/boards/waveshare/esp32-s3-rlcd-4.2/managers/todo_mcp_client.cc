#include "todo_mcp_client.h"

#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sstream>

#include <board.h>
#include <network_interface.h>
#include <settings.h>
#include <web_socket.h>

#include "../config.h"
#include "../ui_limits.h"   // P2 #26：与显示侧共享待办上限常量
#include "memo_store.h"     // 与板级共享 memo/items 读改写锁
#include "weather_manager.h" // 天气统一由 reminder MCP get_weather 提供

// 本地私有配置（含 token，已在 .gitignore 中排除），存在时覆盖 config.h 的默认值
#if __has_include("todo_mcp_secret.h")
#include "todo_mcp_secret.h"
#endif

#define TAG "TodoMcp"

#ifndef MCP_TODO_URL
#define MCP_TODO_URL ""
#endif

// Reminder REST API 直连（不依赖外部 MCP 接入点）；未配置时为空，功能自动关闭。
#ifndef REMINDER_API_URL
#define REMINDER_API_URL ""
#endif
#ifndef REMINDER_API_KEY
#define REMINDER_API_KEY ""
#endif

namespace {
constexpr EventBits_t kRefreshBit = BIT0;
constexpr EventBits_t kInitBit = BIT1;
constexpr EventBits_t kReplyBit = BIT2;
constexpr EventBits_t kHttpRefreshBit = BIT3;

constexpr int kInitTimeoutMs = 10000;
constexpr int kCallTimeoutMs = 20000;
constexpr int kPollMs = 10 * 1000;      // 长连接下的拉取间隔；NVS 仅在内容变化时写入
constexpr int kRetryMs = 5 * 1000;      // 断线/工具路由异常后的重连间隔
constexpr int kFirstDelayMs = 20000;    // 等待网络事件的上限（正常 Connected 会提前唤醒）
constexpr int kHttpFirstDelayMs = 8000; // HTTP 兜底首次拉取（按需启动时使用）
constexpr int kConnectAttempts = 3;     // 建连失败重试次数
constexpr int kConnectGapMs = 1000;     // 首连失败后的重试间隔：缩短空档，减少开机等待

constexpr int kMaxConsecFail = 2;       // 连续拉取失败多少次后重连
constexpr int kListRetryAttempts = 3;   // 单轮 list_todos 失败后的快速重试次数（避免首轮瞬态错误把旧缓存留 60s）
constexpr int kListRetryTimeoutMs = 10000;  // 每次快速重试的响应超时
constexpr int kMaxRows = kTodoFetchMaxRows;  // P2 #26：抓取上限，见 ui_limits.h（显示侧仅渲染 kTodoDisplayMaxItems=6）
constexpr int kWeatherPollMs = 12 * 60 * 60 * 1000;  // 天气成功刷新间隔：12 小时
constexpr int kWeatherRetryMs = 60 * 1000;      // 天气失败重试间隔

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
    // 本地时间尚未同步（RTC/SNTP 未就绪）时 year 会是 1970，不能据此过滤掉全部待办。
    if (tm_now.tm_year + 1900 < 2024) {
        return true;
    }
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

std::string BuildMusicStatusRequest(int id) {
    char buf[192];
    std::snprintf(buf, sizeof(buf),
                  "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"tools/call\",\"params\":"
                  "{\"name\":\"music_status\",\"arguments\":{}}}",
                  id);
    return std::string(buf);
}

std::string BuildListTodosRequest(int id) {
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"tools/call\",\"params\":"
                  "{\"name\":\"list_todos\",\"arguments\":{\"view\":\"today\",\"limit\":10}}}",
                  id);
    return std::string(buf);
}

// reminder MCP 的 get_weather：city 为空时由服务端使用“天气提醒”配置的城市。
// 用 cJSON 构造请求，避免城市名中的特殊字符破坏 JSON。
std::string BuildGetWeatherRequest(int id, const std::string& city) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(root, "id", id);
    cJSON_AddStringToObject(root, "method", "tools/call");
    cJSON* params = cJSON_AddObjectToObject(root, "params");
    cJSON_AddStringToObject(params, "name", "get_weather");
    cJSON* args = cJSON_AddObjectToObject(params, "arguments");
    if (!city.empty()) {
        cJSON_AddStringToObject(args, "city", city.c_str());
    }
    cJSON_AddNumberToObject(args, "days", 3);
    char* out = cJSON_PrintUnformatted(root);
    std::string result = out ? out : "";
    if (out) cJSON_free(out);
    cJSON_Delete(root);
    return result;
}

void LogHeap(const char* when) {
    ESP_LOGI(TAG, "%s：空闲内存 %d 字节，最大连续块 %d 字节", when,
             static_cast<int>(esp_get_free_heap_size()),
             static_cast<int>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
}

struct HttpCtx {
    std::string data;
};

esp_err_t HttpEventHandler(esp_http_client_event_t* evt) {
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->user_data != nullptr &&
        evt->data != nullptr && evt->data_len > 0) {
        static_cast<HttpCtx*>(evt->user_data)->data.append(
            static_cast<const char*>(evt->data), evt->data_len);
    }
    return ESP_OK;
}

// API 返回 due_at 是 UTC（...Z），转换成设备本地时间字符串 "YYYY-MM-DD HH:MM"。
std::string UtcIsoToLocalText(const char* iso, bool all_day) {
    if (iso == nullptr) return std::string();
    int y = 0, mo = 0, d = 0, hh = 0, mm = 0, ss = 0;
    if (std::sscanf(iso, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &hh, &mm, &ss) != 6) {
        return std::string();
    }
    struct tm utc_tm = {};
    utc_tm.tm_year = y - 1900;
    utc_tm.tm_mon = mo - 1;
    utc_tm.tm_mday = d;
    utc_tm.tm_hour = hh;
    utc_tm.tm_min = mm;
    utc_tm.tm_sec = ss;
    utc_tm.tm_isdst = 0;

    time_t now = time(nullptr);
    struct tm local_now = {};
    struct tm utc_now = {};
    localtime_r(&now, &local_now);
    gmtime_r(&now, &utc_now);
    // mktime 按本地时区解释字段；两者之差即本地时区偏移（东八区为 +28800）。
    const long local_offset = static_cast<long>(std::difftime(mktime(&local_now), mktime(&utc_now)));

    time_t as_if_local = mktime(&utc_tm);  // 实际 epoch - offset
    time_t actual = as_if_local + local_offset;
    struct tm local_tm = {};
    localtime_r(&actual, &local_tm);

    char buf[32];
    if (all_day) {
        std::strftime(buf, sizeof(buf), "%Y-%m-%d", &local_tm);
    } else {
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &local_tm);
    }
    return std::string(buf);
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

std::string TodoMcpClient::GetLastMusicTitle() {
    std::lock_guard<std::mutex> lock(mutex_);
    return music_title_;
}

std::string TodoMcpClient::GetLastMusicArtist() {
    std::lock_guard<std::mutex> lock(mutex_);
    return music_artist_;
}

std::string TodoMcpClient::GetLastMusicLyric() {
    std::lock_guard<std::mutex> lock(mutex_);
    return music_lyric_;
}

int TodoMcpClient::GetLastMusicElapsedSec() {
    std::lock_guard<std::mutex> lock(mutex_);
    return music_elapsed_sec_;
}

int TodoMcpClient::GetLastMusicDurationSec() {
    std::lock_guard<std::mutex> lock(mutex_);
    return music_duration_sec_;
}

int64_t TodoMcpClient::GetLastMusicStatusMs() {
    std::lock_guard<std::mutex> lock(mutex_);
    return music_status_ms_;
}

bool TodoMcpClient::ConsumeMusicUpdated() {
    std::lock_guard<std::mutex> lock(mutex_);
    bool updated = music_updated_;
    music_updated_ = false;
    return updated;
}

void TodoMcpClient::RequestRefresh() {
    if (event_group_ != nullptr) {
        xEventGroupSetBits(event_group_, kRefreshBit | kHttpRefreshBit);
    }
}

void TodoMcpClient::TaskEntry(void* arg) {
    static_cast<TodoMcpClient*>(arg)->Run();
}

void TodoMcpClient::HttpTaskEntry(void* arg) {
    static_cast<TodoMcpClient*>(arg)->RunHttp();
}

void TodoMcpClient::RunHttp() {
    vTaskDelay(pdMS_TO_TICKS(kHttpFirstDelayMs));
    for (;;) {
        FetchTodosDirect();
        xEventGroupClearBits(event_group_, kHttpRefreshBit);
        xEventGroupWaitBits(event_group_, kHttpRefreshBit, pdTRUE, pdFALSE, pdMS_TO_TICKS(kPollMs));
    }
}

bool TodoMcpClient::FetchTodosDirect() {
    std::string base(REMINDER_API_URL);
    while (!base.empty() && base.back() == '/') base.pop_back();
    if (base.empty()) return false;

    HttpCtx ctx;
    esp_http_client_config_t config = {};
    const std::string url = base + "/api/reminder/items?view=today&limit=10";
    config.url = url.c_str();
    config.event_handler = HttpEventHandler;
    config.user_data = &ctx;
    config.timeout_ms = 10000;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        ESP_LOGW(TAG, "Reminder HTTP 客户端创建失败");
        return false;
    }
    esp_http_client_set_header(client, "X-API-Key", REMINDER_API_KEY);
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "Reminder HTTP 请求失败 err=%d status=%d len=%d", err, status,
                 static_cast<int>(ctx.data.size()));
        return false;
    }
    return ApplyReminderApiPayload(ctx.data);
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

    // Reminder REST 直连不再开机启动：MCP 长连接已覆盖待办与天气，
    // 同时启动两套 TLS 会争抢启动阶段网络。仅在 MCP 明确缺少 list_todos 时按需启动兜底。
}

void TodoMcpClient::Run() {
    // 等待网络 Connected 事件（RequestRefresh 设置 kRefreshBit）；最多等 kFirstDelayMs 兜底。
    ESP_LOGI(TAG, "等待网络就绪，最多 %d ms", kFirstDelayMs);
    EventBits_t start_bits = xEventGroupWaitBits(event_group_, kRefreshBit, pdTRUE, pdFALSE,
                                                 pdMS_TO_TICKS(kFirstDelayMs));
    if (start_bits & kRefreshBit) {
        ESP_LOGI(TAG, "网络就绪，立即连接 MCP");
    } else {
        ESP_LOGW(TAG, "等待网络事件超时，仍尝试连接 MCP");
    }
    for (;;) {
        const std::string url = GetUrl();
        if (!url.empty()) {
            RunSession(url);
        }
        // 会话结束（断线/未配置），等一会儿或等网络事件再重连。
        // 若服务端明确表示 list_todos 工具不存在且 REST 直连可用，则降为 5 分钟探测一次。
        const int retry_ms = mcp_tool_missing_ ? (5 * 60 * 1000) : kRetryMs;
        xEventGroupClearBits(event_group_, kRefreshBit);
        xEventGroupWaitBits(event_group_, kRefreshBit, pdTRUE, pdFALSE, pdMS_TO_TICKS(retry_ms));
    }
}

bool TodoMcpClient::OpenSession(std::unique_ptr<WebSocket>& ws, const std::string& url) {
    // 待办 MCP 使用带 TLS 会话复用的 SessionNetwork，其余 WebSocket 逻辑不变。
    NetworkInterface* network = &session_network_;

    for (int attempt = 1; attempt <= kConnectAttempts; ++attempt) {
        broken_ = false;
        ws = network->CreateWebSocket(2);
        if (ws == nullptr) {
            ESP_LOGE(TAG, "创建 websocket 失败");
            return false;
        }
        ws->SetReceiveBufferSize(16384);   // 待办列表可能较长，8KB 会截断 JSON
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
    unsigned list_seq = 0;
    int64_t weather_next_ms = 0;   // 首次连上立即取一次天气
    for (;;) {
        if (broken_) {
            ESP_LOGW(TAG, "连接已断开");
            break;
        }

        // 天气优先：会话刚建立就先取 get_weather，避免被 list_todos / music_status 的超时阻塞。
        // 同一条长连接复用；成功 12 小时刷新，失败 60 秒后重试。
        const int64_t now_ms = esp_timer_get_time() / 1000;
        if (now_ms >= weather_next_ms) {
            bool weather_ok = false;
            pending_id_ = 4;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                msg_reply_.clear();
            }
            xEventGroupClearBits(event_group_, kReplyBit);
            const std::string city = WeatherManager::getInstance().city();
            if (ws->Send(BuildGetWeatherRequest(pending_id_, city))) {
                bits = xEventGroupWaitBits(event_group_, kReplyBit, pdTRUE, pdFALSE,
                                           pdMS_TO_TICKS(kCallTimeoutMs));
                if (bits & kReplyBit) {
                    std::string payload;
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        payload = msg_reply_;
                    }
                    weather_ok = ApplyWeatherPayload(payload);
                } else {
                    ESP_LOGW(TAG, "等待 get_weather 响应超时");
                }
            } else {
                ESP_LOGW(TAG, "发送 get_weather 失败");
            }
            weather_next_ms = esp_timer_get_time() / 1000 +
                              (weather_ok ? kWeatherPollMs : kWeatherRetryMs);
        }

        // list_todos 偶发瞬态失败（例如网关首次路由返回 -32601）时，不要直接保留
        // 旧 NVS 并等到 60s 后的下一轮；先在同一会话内快速重试 3 次。
        bool list_ok = false;
        for (int attempt = 1; attempt <= kListRetryAttempts; ++attempt) {
            // 每次重试用唯一 id，避免上一轮迟到的响应被当成这一轮的结果。
            pending_id_ = 100 + static_cast<int>(list_seq++ % 1000);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                msg_reply_.clear();
            }
            xEventGroupClearBits(event_group_, kReplyBit);

            const std::string request = BuildListTodosRequest(pending_id_);
            if (!ws->Send(request)) {
                ESP_LOGW(TAG, "发送 tools/call 失败（第 %d/%d 次）", attempt, kListRetryAttempts);
                break;
            }
            bits = xEventGroupWaitBits(event_group_, kReplyBit, pdTRUE, pdFALSE,
                                       pdMS_TO_TICKS(kListRetryTimeoutMs));
            if (bits & kReplyBit) {
                std::string payload;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    payload = msg_reply_;
                }
                if (ApplyPayload(payload)) {
                    list_ok = true;
                    mcp_tool_missing_ = false;
                    consec_fail = 0;
                    break;
                }
                ESP_LOGW(TAG, "list_todos 返回异常（第 %d/%d 次）", attempt, kListRetryAttempts);
            } else {
                ESP_LOGW(TAG, "等待 list_todos 响应超时（第 %d/%d 次）", attempt, kListRetryAttempts);
            }
            if (attempt < kListRetryAttempts) {
                vTaskDelay(pdMS_TO_TICKS(attempt * 1000));  // 1s、2s 后退避重试
            }
        }
        if (!list_ok) {
            ++consec_fail;
            if (last_list_error_code_ == -32601) {
                // 网关重启/路由表未就绪时会先返回工具不存在；同一会话继续重试也可能命中旧路由，
                // 直接断开重连，让网关重新选择后端 MCP 服务。
                if (std::strlen(REMINDER_API_URL) > 0 && std::strlen(REMINDER_API_KEY) > 0) {
                    mcp_tool_missing_ = true;
                    if (!http_task_started_) {
                        http_task_started_ = true;
                        ESP_LOGI(TAG, "外部 MCP 未提供 list_todos，启动 Reminder REST 兜底");
                        xTaskCreate(HttpTaskEntry, "todo_http", 8192, this, 2, nullptr);
                    }
                    ESP_LOGW(TAG, "外部 MCP 未提供 list_todos，待办改由 Reminder REST 直连提供，5 分钟后重试 MCP");
                } else {
                    ESP_LOGW(TAG, "list_todos 未注册或路由异常，立即重建会话");
                }
                break;
            }
        }
        if (consec_fail >= kMaxConsecFail) {
            ESP_LOGW(TAG, "连续 %d 轮拉取失败，重建连接", consec_fail);
            break;
        }

        // 同一会话里顺带拉一次 music_status，供音乐页空闲时显示上次播放。
        // 失败不影响待办刷新，也不参与 consec_fail 计数。
        pending_id_ = 3;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            msg_reply_.clear();
        }
        xEventGroupClearBits(event_group_, kReplyBit);
        if (ws->Send(BuildMusicStatusRequest(pending_id_))) {
            bits = xEventGroupWaitBits(event_group_, kReplyBit, pdTRUE, pdFALSE,
                                       pdMS_TO_TICKS(kCallTimeoutMs));
            if (bits & kReplyBit) {
                std::string payload;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    payload = msg_reply_;
                }
                ApplyMusicStatusPayload(payload);
            }
        }

        // 等到下一次拉取，期间网络恢复事件可以提前触发
        xEventGroupClearBits(event_group_, kRefreshBit);
        xEventGroupWaitBits(event_group_, kRefreshBit, pdTRUE, pdFALSE, pdMS_TO_TICKS(kPollMs));
    }

    ws->Close();
}

// 待办已独立存 memo/todos；这里只序列化待办数组。
static std::string SerializeTodoArray(cJSON* todos) {
    if (!todos) return "[]";
    char* out = cJSON_PrintUnformatted(todos);
    std::string result = out ? out : "[]";
    if (out) cJSON_free(out);
    return result;
}

bool TodoMcpClient::ApplyPayload(const std::string& payload) {
    if (payload.empty()) return false;

    cJSON* root = cJSON_Parse(payload.c_str());
    if (root == nullptr) return false;

    last_list_error_code_ = 0;

    // 先收进临时行，按时间线排序后再写 NVS（服务端返回顺序不保证时间序）
    struct Row {
        int minutes;            // 排序键：全天 = -1，有具体时间 = 时*60+分，无时间 = 24*60+1
        std::string time_text;  // 显示用：HH:MM / 全天 / 空
        std::string title;
    };
    static Row rows[kMaxRows];  // 任务栈有限，放静态区避免大栈帧
    bool ok = false;
    bool changed = false;
    int count = 0;

    cJSON* error = cJSON_GetObjectItem(root, "error");
    if (error != nullptr) {
        cJSON* code = cJSON_GetObjectItem(error, "code");
        cJSON* message = cJSON_GetObjectItem(error, "message");
        last_list_error_code_ = cJSON_IsNumber(code) ? code->valueint : -1;
        ESP_LOGW(TAG, "待办 MCP 返回错误: code=%d message=%s",
                 last_list_error_code_,
                 (message != nullptr && cJSON_IsString(message) && message->valuestring != nullptr)
                     ? message->valuestring
                     : "(unknown)");
    }

    cJSON* result = cJSON_GetObjectItem(root, "result");
    cJSON* content = (result != nullptr) ? cJSON_GetObjectItem(result, "content") : nullptr;
    cJSON* first = (content != nullptr && cJSON_IsArray(content)) ? cJSON_GetArrayItem(content, 0) : nullptr;
    cJSON* text = (first != nullptr) ? cJSON_GetObjectItem(first, "text") : nullptr;
    if (text == nullptr || !cJSON_IsString(text) || text->valuestring == nullptr) {
        ESP_LOGW(TAG, "待办响应缺少 result.content[0].text");
    }

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
                    const bool is_all_day = cJSON_IsTrue(all_day);
                    std::string tstr = FormatTime(due_text, is_all_day);
                    int minutes = 24 * 60 + 1;          // 无时间 → 排最后
                    if (is_all_day) {
                        minutes = -1;                    // 全天 → 排最前
                    } else if (std::strlen(due_text) >= 16) {
                        int hh = 0, mm = 0;
                        if (std::sscanf(due_text + 11, "%d:%d", &hh, &mm) == 2) {
                            minutes = hh * 60 + mm;
                        }
                    }
                    rows[count].minutes = minutes;
                    rows[count].time_text = tstr;
                    rows[count].title = title->valuestring;
                    count++;
                }
            }
            cJSON_Delete(inner);
        }
    }
    cJSON_Delete(root);

    // 按时间线升序（std::stable_sort 保证同一时间的多条保持服务端原顺序）
    std::stable_sort(rows, rows + count, [](const Row& a, const Row& b) {
        return a.minutes < b.minutes;
    });

    cJSON* arr = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        ESP_LOGI(TAG, "待办行: %s %s", rows[i].time_text.c_str(), rows[i].title.c_str());
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "t", rows[i].time_text.c_str());
        cJSON_AddStringToObject(item, "c", rows[i].title.c_str());
        cJSON_AddStringToObject(item, "type", "todo");
        cJSON_AddItemToArray(arr, item);
    }

    if (!ok) {
        cJSON_Delete(arr);
        ESP_LOGW(TAG, "待办服务返回失败，保留原列表");
        return false;
    }

    // 只替换待办部分，保留手动备忘；10 秒轮询下内容未变化时不写 NVS。
    std::lock_guard<std::mutex> memo_lock(MemoStoreMutex());
    std::lock_guard<std::mutex> nvs_lock(nvs_mutex_);
    Settings read_settings(kMemoNvsNamespace, false);
    const std::string current = read_settings.GetString(kMemoTodoKey, "");
    const std::string next = SerializeTodoArray(arr);
    changed = (current != next);
    if (changed) {
        Settings write_settings(kMemoNvsNamespace, true);
        write_settings.SetString(kMemoTodoKey, next.c_str());
    }
    cJSON_Delete(arr);

    ESP_LOGI(TAG, "已拉取当日待办 %d 条（%s）", count, changed ? "有更新" : "无变化");
    if (changed && on_updated_) {
        on_updated_();
    }
    return true;
}


bool TodoMcpClient::ApplyReminderApiPayload(const std::string& payload) {
    if (payload.empty()) return false;
    cJSON* root = cJSON_Parse(payload.c_str());
    if (root == nullptr) return false;

    cJSON* code = cJSON_GetObjectItem(root, "code");
    if (!cJSON_IsNumber(code) || code->valueint != 0) {
        ESP_LOGW(TAG, "Reminder HTTP 返回错误: code=%d", cJSON_IsNumber(code) ? code->valueint : -1);
        cJSON_Delete(root);
        return false;
    }
    cJSON* data = cJSON_GetObjectItem(root, "data");
    if (data == nullptr || !cJSON_IsArray(data)) {
        ESP_LOGW(TAG, "Reminder HTTP 响应缺少 data 数组");
        cJSON_Delete(root);
        return false;
    }

    struct Row {
        int minutes;
        std::string time_text;
        std::string title;
    };
    static Row rows[kMaxRows];
    int count = 0;

    cJSON* it = nullptr;
    cJSON_ArrayForEach(it, data) {
        if (count >= kMaxRows) break;
        cJSON* title = cJSON_GetObjectItem(it, "title");
        if (title == nullptr || !cJSON_IsString(title) || title->valuestring == nullptr ||
            title->valuestring[0] == 0) {
            continue;
        }
        cJSON* due = cJSON_GetObjectItem(it, "due_at");
        cJSON* all_day = cJSON_GetObjectItem(it, "all_day");
        const char* due_at = (due != nullptr && cJSON_IsString(due) && due->valuestring != nullptr)
                                 ? due->valuestring
                                 : "";
        const bool is_all_day = cJSON_IsTrue(all_day);
        std::string due_text;
        int minutes = 24 * 60 + 1;
        if (due_at[0] != 0) {
            due_text = UtcIsoToLocalText(due_at, is_all_day);
            if (due_text.empty() || !IsToday(due_text.c_str())) {
                continue;
            }
            if (is_all_day) {
                minutes = -1;
            } else {
                int hh = 0, mm = 0;
                if (std::sscanf(due_text.c_str() + 11, "%d:%d", &hh, &mm) == 2) {
                    minutes = hh * 60 + mm;
                }
            }
        }
        rows[count].minutes = minutes;
        rows[count].time_text = is_all_day ? "全天" : FormatTime(due_text.c_str(), false);
        rows[count].title = title->valuestring;
        count++;
    }
    cJSON_Delete(root);

    std::stable_sort(rows, rows + count, [](const Row& a, const Row& b) {
        return a.minutes < b.minutes;
    });

    cJSON* arr = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        ESP_LOGI(TAG, "待办行: %s %s", rows[i].time_text.c_str(), rows[i].title.c_str());
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "t", rows[i].time_text.c_str());
        cJSON_AddStringToObject(item, "c", rows[i].title.c_str());
        cJSON_AddStringToObject(item, "type", "todo");
        cJSON_AddItemToArray(arr, item);
    }

    bool changed = false;
    // 只替换待办部分，保留手动备忘
    std::lock_guard<std::mutex> memo_lock(MemoStoreMutex());
    std::lock_guard<std::mutex> nvs_lock(nvs_mutex_);
    Settings read_settings(kMemoNvsNamespace, false);
    const std::string current = read_settings.GetString(kMemoTodoKey, "");
    const std::string next = SerializeTodoArray(arr);
    changed = (current != next);
    if (changed) {
        Settings write_settings(kMemoNvsNamespace, true);
        write_settings.SetString(kMemoTodoKey, next.c_str());
    }
    cJSON_Delete(arr);

    ESP_LOGI(TAG, "Reminder HTTP 拉取当日待办 %d 条（%s）", count, changed ? "有更新" : "无变化");
    if (changed && on_updated_) {
        on_updated_();
    }
    return true;
}

static bool StartsWith(const std::string& s, const char* prefix) {
    size_t n = std::strlen(prefix);
    return s.size() >= n && s.compare(0, n, prefix) == 0;
}

static std::string LineAfter(const std::string& raw, const char* prefix) {
    std::istringstream in(raw);
    std::string line;
    const size_t n = std::strlen(prefix);
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (StartsWith(line, prefix)) return line.substr(n);
    }
    return "";
}

// 把 "123 秒" 解析成 123；不是数字返回 -1。
static int ParseSeconds(const std::string& text) {
    if (text.empty()) return -1;
    char* end = nullptr;
    long v = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || v < 0) return -1;
    return static_cast<int>(v);
}

bool TodoMcpClient::ApplyWeatherPayload(const std::string& payload) {
    return WeatherManager::getInstance().updateFromReminderJson(payload);
}

bool TodoMcpClient::ApplyMusicStatusPayload(const std::string& payload) {
    if (payload.empty()) return false;
    cJSON* root = cJSON_Parse(payload.c_str());
    if (root == nullptr) return false;

    cJSON* result = cJSON_GetObjectItem(root, "result");
    cJSON* content = (result != nullptr) ? cJSON_GetObjectItem(result, "content") : nullptr;
    cJSON* first = (content != nullptr && cJSON_IsArray(content)) ? cJSON_GetArrayItem(content, 0) : nullptr;
    cJSON* text = (first != nullptr) ? cJSON_GetObjectItem(first, "text") : nullptr;

    bool changed = false;
    if (text != nullptr && cJSON_IsString(text) && text->valuestring != nullptr) {
        const std::string raw = text->valuestring;
        std::string title, artist, lyric;
        int elapsed = -1, duration = -1;

        if (StartsWith(raw, "正在播放：")) {
            std::string head = LineAfter(raw, "正在播放：");
            size_t dash = head.find(" - ");
            title = (dash == std::string::npos) ? head : head.substr(0, dash);
            artist = (dash == std::string::npos) ? "" : head.substr(dash + 3);
            lyric = LineAfter(raw, "歌词：");
            elapsed = ParseSeconds(LineAfter(raw, "已播放："));
            duration = ParseSeconds(LineAfter(raw, "时长："));
        } else if (StartsWith(raw, "上一次播放：")) {
            std::string rest = raw.substr(std::strlen("上一次播放："));
            size_t cut = rest.find("（");
            if (cut == std::string::npos) cut = rest.find('(');
            if (cut != std::string::npos) rest = rest.substr(0, cut);
            size_t dash = rest.find(" - ");
            if (dash != std::string::npos) {
                title = rest.substr(0, dash);
                artist = rest.substr(dash + 3);
            }
        }

        if (!title.empty()) {
            const int64_t now_ms = esp_timer_get_time() / 1000;
            std::lock_guard<std::mutex> lock(mutex_);
            if (title != music_title_ || artist != music_artist_ || lyric != music_lyric_ ||
                (duration >= 0 && duration != music_duration_sec_)) {
                music_title_ = title;
                music_artist_ = artist;
                music_lyric_ = lyric;
                if (elapsed >= 0) music_elapsed_sec_ = elapsed;
                if (duration >= 0) music_duration_sec_ = duration;
                music_status_ms_ = now_ms;
                music_updated_ = true;
                changed = true;
            }
        }
    }
    cJSON_Delete(root);
    if (changed) {
        ESP_LOGI(TAG, "音乐元数据: %s - %s / %s", GetLastMusicTitle().c_str(),
                 GetLastMusicArtist().c_str(), GetLastMusicLyric().c_str());
    }
    return changed;
}
