#ifndef SESSION_TLS_H
#define SESSION_TLS_H

#include "tcp.h"
#include "esp_network.h"

#include <esp_tls.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>

#include <memory>
#include <mutex>
#include <string>

// Same event bit/timeout contract as EspSsl, kept local so this file does not
// depend on the managed component private header.
constexpr EventBits_t kSessionSslReceiveTaskExit = 1;
constexpr int kSessionSslSendTimeoutMs = 10000;

// WebSocket/TLS transport with a process-local and NVS-persisted TLS session.
// The MCP endpoint supports TLS 1.2 session tickets; reusing the ticket avoids
// a full certificate-chain/key-exchange handshake after a reboot or reconnect.
class SessionSsl : public Tcp {
public:
    SessionSsl();
    ~SessionSsl() override;

    NetworkResult<> Connect(const std::string& host, int port) override;
    void Disconnect() override;
    int Send(const std::string& data) override;

private:
    void ReceiveTask();
    void LoadPersistedSession(const std::string& host);
    void SavePersistedSession(const std::string& host);
    void FreeClientSession();

    esp_tls_t* tls_client_ = nullptr;
    int sockfd_ = -1;
    std::mutex tls_mutex_;
    EventGroupHandle_t event_group_ = nullptr;
    TaskHandle_t receive_task_handle_ = nullptr;
#ifdef CONFIG_ESP_TLS_CLIENT_SESSION_TICKETS
    esp_tls_client_session_t* client_session_ = nullptr;
#endif
    std::string session_host_;
};

// EspNetwork 的普通 CreateSsl 仍返回无会话复用的 EspSsl；待办 MCP 专用
// 的 SessionNetwork 只覆盖 CreateSsl，WebSocket 的其余逻辑保持不变。
class SessionNetwork : public EspNetwork {
public:
    std::unique_ptr<Tcp> CreateSsl(int connect_id = -1) override;
};

#endif  // SESSION_TLS_H
