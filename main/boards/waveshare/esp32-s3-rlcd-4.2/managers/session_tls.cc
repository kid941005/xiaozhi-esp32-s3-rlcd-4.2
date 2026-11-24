#include "session_tls.h"

#include <esp_crt_bundle.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <mbedtls/base64.h>
#include <mbedtls/ssl.h>
#include <settings.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

#define TAG "SessionTls"

namespace {

constexpr char kSessionNs[] = "tls_session";
constexpr char kSessionKey[] = "mcp";

// Wait until fd is readable and/or writable. A negative timeout waits forever.
int WaitSocket(int fd, bool for_read, bool for_write, int timeout_ms) {
    fd_set read_fds;
    fd_set write_fds;
    FD_ZERO(&read_fds);
    FD_ZERO(&write_fds);
    if (for_read) {
        FD_SET(fd, &read_fds);
    }
    if (for_write) {
        FD_SET(fd, &write_fds);
    }
    struct timeval tv = {};
    if (timeout_ms >= 0) {
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
    }
    return select(fd + 1, for_read ? &read_fds : nullptr, for_write ? &write_fds : nullptr, nullptr,
                  timeout_ms >= 0 ? &tv : nullptr);
}

std::string Base64Encode(const unsigned char* data, size_t len) {
    if (data == nullptr || len == 0) {
        return {};
    }
    const size_t out_len = 4 * ((len + 2) / 3) + 1;
    std::vector<unsigned char> out(out_len);
    size_t written = 0;
    if (mbedtls_base64_encode(out.data(), out.size(), &written, data, len) != 0) {
        return {};
    }
    return std::string(reinterpret_cast<const char*>(out.data()), written);
}

bool Base64Decode(const std::string& text, std::vector<unsigned char>& out) {
    if (text.empty()) {
        return false;
    }
    out.assign(text.size() / 4 * 3 + 3, 0);
    size_t written = 0;
    if (mbedtls_base64_decode(out.data(), out.size(), &written,
                             reinterpret_cast<const unsigned char*>(text.data()), text.size()) != 0) {
        return false;
    }
    out.resize(written);
    return true;
}

}  // namespace

SessionSsl::SessionSsl() {
    event_group_ = xEventGroupCreate();
}

SessionSsl::~SessionSsl() {
    Disconnect();
    FreeClientSession();

    if (event_group_ != nullptr) {
        vEventGroupDelete(event_group_);
        event_group_ = nullptr;
    }
}

void SessionSsl::FreeClientSession() {
#ifdef CONFIG_ESP_TLS_CLIENT_SESSION_TICKETS
    if (client_session_ != nullptr) {
        esp_tls_free_client_session(client_session_);
        client_session_ = nullptr;
    }
#endif
    session_host_.clear();
}

void SessionSsl::LoadPersistedSession(const std::string& host) {
#ifdef CONFIG_ESP_TLS_CLIENT_SESSION_TICKETS
    if (client_session_ != nullptr || host.empty()) {
        return;
    }

    Settings settings(kSessionNs, false);
    const std::string stored = settings.GetString(kSessionKey);
    if (stored.empty()) {
        return;
    }
    const size_t split = stored.find('\n');
    if (split == std::string::npos || stored.substr(0, split) != host) {
        return;
    }

    std::vector<unsigned char> bytes;
    if (!Base64Decode(stored.substr(split + 1), bytes) || bytes.empty()) {
        ESP_LOGW(TAG, "NVS 中的 TLS 会话无法解码，忽略");
        return;
    }

    auto* loaded = static_cast<esp_tls_client_session_t*>(calloc(1, sizeof(esp_tls_client_session_t)));
    if (loaded == nullptr) {
        return;
    }
    mbedtls_ssl_session_init(&loaded->saved_session);
    const int ret = mbedtls_ssl_session_load(&loaded->saved_session, bytes.data(), bytes.size());
    if (ret != 0) {
        ESP_LOGW(TAG, "TLS 会话恢复失败: -0x%04X，改为完整握手", -ret);
        mbedtls_ssl_session_free(&loaded->saved_session);
        free(loaded);
        return;
    }
    client_session_ = loaded;
    session_host_ = host;
    ESP_LOGI(TAG, "已从 NVS 恢复 TLS 会话 (%u 字节)，本机尝试复用", (unsigned)bytes.size());
#else
    (void)host;
#endif
}

void SessionSsl::SavePersistedSession(const std::string& host) {
#ifdef CONFIG_ESP_TLS_CLIENT_SESSION_TICKETS
    if (client_session_ == nullptr || host.empty()) {
        return;
    }

    std::vector<unsigned char> serialized(4096);
    size_t written = 0;
    int ret = mbedtls_ssl_session_save(&client_session_->saved_session, serialized.data(),
                                       serialized.size(), &written);
    if (ret == MBEDTLS_ERR_SSL_BUFFER_TOO_SMALL) {
        serialized.resize(written);
        ret = mbedtls_ssl_session_save(&client_session_->saved_session, serialized.data(),
                                       serialized.size(), &written);
    }
    if (ret != 0) {
        ESP_LOGW(TAG, "TLS 会话序列化失败: -0x%04X，本次不持久化", -ret);
        return;
    }

    const std::string encoded = Base64Encode(serialized.data(), written);
    if (encoded.empty()) {
        return;
    }
    Settings settings(kSessionNs, true);
    settings.SetString(kSessionKey, host + "\n" + encoded);
    session_host_ = host;
    ESP_LOGI(TAG, "TLS 会话已写入 NVS (%u 字节)", (unsigned)written);
#else
    (void)host;
#endif
}

NetworkResult<> SessionSsl::Connect(const std::string& host, int port) {
    // Also releases the TLS context and joins the receive task left by a
    // passive disconnect, so the object can be reconnected.
    Disconnect();

    // 连接不同主机时不能复用旧主机的 ticket。
    if (!session_host_.empty() && session_host_ != host) {
        FreeClientSession();
    }
    LoadPersistedSession(host);
    const bool had_session =
#ifdef CONFIG_ESP_TLS_CLIENT_SESSION_TICKETS
        client_session_ != nullptr;
#else
        false;
#endif

    tls_client_ = esp_tls_init();
    if (tls_client_ == nullptr) {
        ESP_LOGE(TAG, "Failed to initialize TLS");
        return Fail(NetworkError::TlsFailed());
    }

    auto destroy_tls = [this]() {
        esp_tls_conn_destroy(tls_client_);
        tls_client_ = nullptr;
        sockfd_ = -1;
    };

    esp_tls_cfg_t cfg = {};
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    // TLS 1.2 tickets are supported by the MCP endpoint; keep the session
    // reusable instead of negotiating TLS 1.3 session tickets.
    cfg.tls_version = ESP_TLS_VER_TLS_1_2;
#ifdef CONFIG_ESP_TLS_CLIENT_SESSION_TICKETS
    cfg.client_session = client_session_;
#endif

    const int64_t start_us = esp_timer_get_time();
    int ret = esp_tls_conn_new_sync(host.c_str(), host.length(), port, &cfg, tls_client_);
    if (ret != 1) {
        NetworkError err = NetworkError::TlsFailed();
        esp_tls_error_handle_t tls_error;
        if (esp_tls_get_error_handle(tls_client_, &tls_error) == ESP_OK) {
            int error_code, error_flags;
            esp_err_t esp_err = esp_tls_get_and_clear_last_error(tls_error, &error_code, &error_flags);
            err = NetworkError::FromEsp(esp_err);
            ESP_LOGE(TAG, "Failed to connect to %s:%d: %s", host.c_str(), port, err.ToString().c_str());
        } else {
            ESP_LOGE(TAG, "Failed to get error handle");
        }
        // A stale ticket can be rejected; drop both the in-memory and NVS copy
        // so the next attempt does a full handshake instead of repeatedly
        // failing with the same ticket.
        if (had_session) {
            Settings settings(kSessionNs, true);
            settings.EraseKey(kSessionKey);
        }
        FreeClientSession();
        destroy_tls();
        return Fail(err);
    }

    int sockfd = -1;
    if (esp_tls_get_conn_sockfd(tls_client_, &sockfd) != ESP_OK || sockfd < 0) {
        ESP_LOGE(TAG, "Failed to get socket fd");
        destroy_tls();
        return Fail(NetworkError::TlsFailed());
    }
    int flags = fcntl(sockfd, F_GETFL, 0);
    if (flags < 0 || fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) < 0) {
        int err = errno;
        ESP_LOGE(TAG, "Failed to set socket non-blocking: errno=%d", err);
        destroy_tls();
        return Fail(NetworkError::FromErrno(err));
    }
    sockfd_ = sockfd;

#ifdef CONFIG_ESP_TLS_CLIENT_SESSION_TICKETS
    // Take ownership of a copy of the negotiated session before the receive
    // task can start. For TLS 1.2 esp_tls returns a deep copy.
    esp_tls_client_session_t* negotiated = esp_tls_get_client_session(tls_client_);
    if (negotiated != nullptr) {
        if (client_session_ != nullptr) {
            esp_tls_free_client_session(client_session_);
        }
        client_session_ = negotiated;
        SavePersistedSession(host);
    }
#endif

    ESP_LOGI(TAG, "TLS 握手完成 %lld ms（%s）", (long long)((esp_timer_get_time() - start_us) / 1000),
             had_session ? "复用会话" : "完整握手");

    connected_ = true;
    xEventGroupClearBits(event_group_, kSessionSslReceiveTaskExit);
    BaseType_t rc = xTaskCreate([](void* arg) {
        SessionSsl* ssl = static_cast<SessionSsl*>(arg);
        ssl->ReceiveTask();
        xEventGroupSetBits(ssl->event_group_, kSessionSslReceiveTaskExit);
        vTaskDelete(NULL);
    }, "ssl_receive", 4096, this, 1, &receive_task_handle_);
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "Failed to create receive task");
        connected_ = false;
        receive_task_handle_ = nullptr;
        destroy_tls();
        return Fail(NetworkError::FromErrno(ENOMEM));
    }
    return {};
}

void SessionSsl::Disconnect() {
    bool was_connected = connected_.exchange(false);
    if (tls_client_ == nullptr) {
        return;
    }

    // Use shutdown(), not close(). close() hands the fd number back to lwIP
    // immediately, and esp_tls_conn_destroy() below closes the same number
    // again. shutdown() still wakes ReceiveTask, but keeps the fd so
    // esp_tls_conn_destroy() closes it exactly once.
    if (sockfd_ >= 0) {
        shutdown(sockfd_, SHUT_RDWR);
    }

    if (receive_task_handle_ != nullptr) {
        auto bits = xEventGroupWaitBits(event_group_, kSessionSslReceiveTaskExit, pdFALSE, pdFALSE,
                                        pdMS_TO_TICKS(10000));
        if (!(bits & kSessionSslReceiveTaskExit)) {
            ESP_LOGE(TAG, "Failed to wait for receive task exit");
        }
        receive_task_handle_ = nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(tls_mutex_);
        esp_tls_conn_destroy(tls_client_);
        tls_client_ = nullptr;
        sockfd_ = -1;
    }

    if (was_connected && disconnect_callback_) {
        disconnect_callback_();
    }
}

int SessionSsl::Send(const std::string& data) {
    if (!connected_) {
        ESP_LOGE(TAG, "Not connected");
        return -1;
    }

    size_t total_sent = 0;
    size_t data_size = data.size();
    const char* data_ptr = data.data();
    const TickType_t timeout_ticks = pdMS_TO_TICKS(kSessionSslSendTimeoutMs);
    TickType_t last_progress = xTaskGetTickCount();

    while (total_sent < data_size) {
        int ret;
        int sockfd;
        {
            std::lock_guard<std::mutex> lock(tls_mutex_);
            if (tls_client_ == nullptr || !connected_) {
                ESP_LOGE(TAG, "Not connected");
                return -1;
            }
            // After WANT_WRITE mbedTLS requires the same arguments on retry,
            // which holds because total_sent only advances on success.
            ret = esp_tls_conn_write(tls_client_, data_ptr + total_sent, data_size - total_sent);
            sockfd = sockfd_;
        }

        if (ret > 0) {
            total_sent += ret;
            last_progress = xTaskGetTickCount();
            continue;
        }

        if (ret != ESP_TLS_ERR_SSL_WANT_WRITE && ret != ESP_TLS_ERR_SSL_WANT_READ) {
            ESP_LOGE(TAG, "SSL send failed: ret=%d, errno=%d", ret, errno);
            return ret;
        }

        TickType_t elapsed = xTaskGetTickCount() - last_progress;
        if (elapsed >= timeout_ticks) {
            ESP_LOGE(TAG, "SSL send timeout, sent %u of %u bytes", (unsigned)total_sent, (unsigned)data_size);
            return -1;
        }
        if (ret == ESP_TLS_ERR_SSL_WANT_WRITE) {
            WaitSocket(sockfd, false, true, (timeout_ticks - elapsed) * portTICK_PERIOD_MS);
        } else {
            // The record mbedTLS waits for is read by ReceiveTask, so poll
            // instead of competing with it in select().
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    return total_sent;
}

void SessionSsl::ReceiveTask() {
    std::string data;
    bool want_write = false;
    while (connected_) {
        bool pending;
        {
            std::lock_guard<std::mutex> lock(tls_mutex_);
            pending = esp_tls_get_bytes_avail(tls_client_) > 0;
        }
        // Decrypted bytes may remain in mbedTLS when the record is larger than
        // the read buffer; the socket would not signal readability for them.
        if (!pending) {
            // Woken by incoming data, peer close or reset, or shutdown() in Disconnect()
            int ret = WaitSocket(sockfd_, true, want_write, -1);
            if (ret < 0 && errno != EINTR) {
                ESP_LOGE(TAG, "select failed: errno=%d", errno);
            }
            if (!connected_) {
                break;
            }
        }

        data.resize(1500);
        int ret;
        {
            std::lock_guard<std::mutex> lock(tls_mutex_);
            ret = esp_tls_conn_read(tls_client_, data.data(), data.size());
        }

        if (ret == ESP_TLS_ERR_SSL_WANT_READ || ret == ESP_TLS_ERR_SSL_WANT_WRITE) {
            want_write = ret == ESP_TLS_ERR_SSL_WANT_WRITE;
            continue;
        }
        want_write = false;

        if (ret <= 0) {
            // Passive disconnect: only report it, Disconnect() releases the TLS
            // context. exchange() keeps the callback to one call when
            // Disconnect() races.
            if (connected_.exchange(false)) {
                if (ret < 0) {
                    ESP_LOGE(TAG, "SSL receive failed: %d", ret);
                }
                if (disconnect_callback_) {
                    disconnect_callback_();
                }
            }
            break;
        }

        if (stream_callback_) {
            data.resize(ret);
            stream_callback_(data);
        }
    }
}

std::unique_ptr<Tcp> SessionNetwork::CreateSsl(int connect_id) {
    (void)connect_id;
    return std::make_unique<SessionSsl>();
}
