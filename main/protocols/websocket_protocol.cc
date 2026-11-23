#include "websocket_protocol.h"
#include "application.h"
#include "board.h"
#include "settings.h"
#include "system_info.h"

#include <esp_log.h>
#include <arpa/inet.h>
#include <cJSON.h>
#include <cstring>
#include "assets/lang_config.h"

#define TAG "WS"

WebsocketProtocol::WebsocketProtocol() {
    event_group_handle_ = xEventGroupCreate();
    esp_timer_create_args_t args = {};
    args.callback = [](void* arg) { static_cast<WebsocketProtocol*>(arg)->PingTimerCallback(); };
    args.arg = this;
    args.name = "ws_ping";
    if (esp_timer_create(&args, &ping_timer_) != ESP_OK) {
        ping_timer_ = nullptr;
    }
}

WebsocketProtocol::~WebsocketProtocol() {
    if (ping_timer_ != nullptr) {
        esp_timer_stop(ping_timer_);
        esp_timer_delete(ping_timer_);
        ping_timer_ = nullptr;
    }
    vEventGroupDelete(event_group_handle_);
}

bool WebsocketProtocol::Start() {
    // Only connect to server when audio channel is needed
    return true;
}

bool WebsocketProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    if (websocket_ == nullptr || !websocket_->IsConnected()) {
        return false;
    }

    if (version_ == 2) {
        std::string serialized;
        serialized.resize(sizeof(BinaryProtocol2) + packet->payload.size());
        auto bp2 = (BinaryProtocol2*)serialized.data();
        bp2->version = htons(version_);
        bp2->type = 0;
        bp2->reserved = 0;
        bp2->timestamp = htonl(packet->timestamp);
        bp2->payload_size = htonl(packet->payload.size());
        memcpy(bp2->payload, packet->payload.data(), packet->payload.size());

        return websocket_->Send(serialized.data(), serialized.size(), true);
    } else if (version_ == 3) {
        std::string serialized;
        serialized.resize(sizeof(BinaryProtocol3) + packet->payload.size());
        auto bp3 = (BinaryProtocol3*)serialized.data();
        bp3->type = 0;
        bp3->reserved = 0;
        bp3->payload_size = htons(packet->payload.size());
        memcpy(bp3->payload, packet->payload.data(), packet->payload.size());

        return websocket_->Send(serialized.data(), serialized.size(), true);
    } else {
        return websocket_->Send(packet->payload.data(), packet->payload.size(), true);
    }
}

bool WebsocketProtocol::SendText(const std::string& text) {
    if (websocket_ == nullptr || !websocket_->IsConnected()) {
        return false;
    }

    if (!websocket_->Send(text)) {
        ESP_LOGE(TAG, "Failed to send text: %s", text.c_str());
        SetError(Lang::Strings::SERVER_ERROR);
        return false;
    }

    return true;
}

bool WebsocketProtocol::IsAudioChannelOpened() const {
    return websocket_ != nullptr && websocket_->IsConnected() && !error_occurred_ && !IsTimeout();
}

void WebsocketProtocol::CloseAudioChannel(bool send_goodbye) {
    (void)send_goodbye;  // Websocket doesn't need to send goodbye message
    if (ping_timer_ != nullptr) {
        esp_timer_stop(ping_timer_);
    }
    websocket_.reset();
}

bool WebsocketProtocol::OpenAudioChannel() {
    Settings settings("websocket", false);
    std::string url = settings.GetString("url");
    std::string token = settings.GetString("token");
    int version = settings.GetInt("version");
    version_ = (version == 1 || version == 2 || version == 3) ? version : 1;
    // The v1 endpoint uses raw Opus frames; a stale v2/v3 value would make
    // both uplink encoding and downlink parsing incompatible with the server.
    if (url.find("/v1/") != std::string::npos && version_ != 1) {
        ESP_LOGW(TAG, "Server URL is v1; overriding stored protocol version %d with 1", version_);
        version_ = 1;
    }

    error_occurred_ = false;

    auto network = Board::GetInstance().GetNetwork();
    websocket_ = network->CreateWebSocket(1);
    if (websocket_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create websocket");
        return false;
    }

    if (!token.empty()) {
        // If token not has a space, add "Bearer " prefix
        if (token.find(" ") == std::string::npos) {
            token = "Bearer " + token;
        }
        websocket_->SetHeader("Authorization", token.c_str());
    }
    websocket_->SetHeader("Protocol-Version", std::to_string(version_).c_str());
    websocket_->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    websocket_->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());

    websocket_->OnData([this](const char* data, size_t len, bool binary) {
        if (binary) {
            if (on_incoming_audio_ != nullptr) {
                if (version_ == 2) {
                    if (len < sizeof(BinaryProtocol2)) {
                        ESP_LOGW(TAG, "Invalid binary frame: len=%u, need at least %u", (unsigned)len,
                                 (unsigned)sizeof(BinaryProtocol2));
                        return;
                    }
                    BinaryProtocol2 header;
                    memcpy(&header, data, sizeof(header));
                    header.timestamp = ntohl(header.timestamp);
                    header.payload_size = ntohl(header.payload_size);
                    if (header.payload_size > len - sizeof(BinaryProtocol2)) {
                        ESP_LOGW(TAG, "Invalid binary frame: payload_size=%u exceeds available=%u",
                                 (unsigned)header.payload_size,
                                 (unsigned)(len - sizeof(BinaryProtocol2)));
                        return;
                    }
                    auto payload = reinterpret_cast<const uint8_t*>(data) + sizeof(BinaryProtocol2);
                    on_incoming_audio_(std::make_unique<AudioStreamPacket>(AudioStreamPacket{
                        .sample_rate = server_sample_rate_,
                        .frame_duration = server_frame_duration_,
                        .timestamp = header.timestamp,
                        .payload = std::vector<uint8_t>(payload, payload + header.payload_size)}));
                } else if (version_ == 3) {
                    if (len < sizeof(BinaryProtocol3)) {
                        ESP_LOGW(TAG, "Invalid binary frame: len=%u, need at least %u", (unsigned)len,
                                 (unsigned)sizeof(BinaryProtocol3));
                        return;
                    }
                    BinaryProtocol3 header;
                    memcpy(&header, data, sizeof(header));
                    header.payload_size = ntohs(header.payload_size);
                    if (header.payload_size > len - sizeof(BinaryProtocol3)) {
                        ESP_LOGW(TAG, "Invalid binary frame: payload_size=%u exceeds available=%u",
                                 (unsigned)header.payload_size,
                                 (unsigned)(len - sizeof(BinaryProtocol3)));
                        return;
                    }
                    auto payload = reinterpret_cast<const uint8_t*>(data) + sizeof(BinaryProtocol3);
                    on_incoming_audio_(std::make_unique<AudioStreamPacket>(AudioStreamPacket{
                        .sample_rate = server_sample_rate_,
                        .frame_duration = server_frame_duration_,
                        .timestamp = 0,
                        .payload = std::vector<uint8_t>(payload, payload + header.payload_size)}));
                } else {
                    on_incoming_audio_(std::make_unique<AudioStreamPacket>(AudioStreamPacket{
                        .sample_rate = server_sample_rate_,
                        .frame_duration = server_frame_duration_,
                        .timestamp = 0,
                        .payload = std::vector<uint8_t>((uint8_t*)data, (uint8_t*)data + len)}));
                }
            }
        } else {
            // Parse JSON data
            auto root = cJSON_ParseWithLength(data, len);
            auto type = cJSON_GetObjectItem(root, "type");
            if (cJSON_IsString(type)) {
                if (strcmp(type->valuestring, "hello") == 0) {
                    ParseServerHello(root);
                } else {
                    if (on_incoming_json_ != nullptr) {
                        on_incoming_json_(root);
                    }
                }
            } else {
                ESP_LOGE(TAG, "Missing message type, data: %s", std::string(data, len).c_str());
            }
            cJSON_Delete(root);
        }
        MarkIncomingTime();
    });

    // Callbacks run on the transport's receive task, which on 4G modems is the
    // AT event task and cannot send, so the Pong is sent from the main task.
    std::weak_ptr<WebSocket> weak_websocket = websocket_;
    websocket_->OnPing([weak_websocket](const char* data, size_t len) {
        Application::GetInstance().Schedule([weak_websocket, payload = std::string(data, len)]() {
            if (auto websocket = weak_websocket.lock()) {
                websocket->SendPong(payload.data(), payload.size());
            }
        });
    });

    websocket_->OnPong([this](const char*, size_t) {
        // 收到 Pong 说明链路仍然活跃，刷新超时基准。
        MarkIncomingTime();
    });

    websocket_->OnDisconnected([this]() {
        ESP_LOGI(TAG, "Websocket disconnected");
        if (ping_timer_ != nullptr) {
            esp_timer_stop(ping_timer_);
        }
        if (on_audio_channel_closed_ != nullptr) {
            on_audio_channel_closed_();
        }
    });

    ESP_LOGI(TAG, "Connecting to websocket server: %s with version: %d", url.c_str(), version_);
    // 一次连接超时并不代表服务不可用：唤醒时电源管理重新配置或链路瞬时抖动
    // 都可能让首次 TCP/TLS 握手超时，而紧接着重试通常就能成功。这里先重试
    // 一次，避免把一次瞬时失败变成常驻的“无法连接服务”告警。
    bool connected = false;
    std::string last_error;
    for (int attempt = 1; attempt <= 2; ++attempt) {
        auto result = websocket_->Connect(url.c_str());
        if (result) {
            connected = true;
            break;
        }
        last_error = result.error().ToString();
        ESP_LOGW(TAG, "Failed to connect to websocket server (attempt %d/2): %s",
                 attempt, last_error.c_str());
        if (attempt < 2) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
    if (!connected) {
        ESP_LOGE(TAG, "Failed to connect to websocket server: %s", last_error.c_str());
        SetError(Lang::Strings::SERVER_NOT_CONNECTED, url);
        return false;
    }

    // Send hello message to describe the client
    auto message = GetHelloMessage();
    if (!SendText(message)) {
        CloseAudioChannel(false);
        return false;
    }

    // Wait for server hello
    EventBits_t bits =
        xEventGroupWaitBits(event_group_handle_, WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT, pdTRUE,
                            pdFALSE, pdMS_TO_TICKS(10000));
    if (!(bits & WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT)) {
        ESP_LOGE(TAG, "Failed to receive server hello");
        SetError(Lang::Strings::SERVER_TIMEOUT);
        // 连接已建立但协议握手失败，必须关闭底层 websocket，避免泄漏连接/接收任务。
        CloseAudioChannel(false);
        return false;
    }

    // 连接成功后通知应用层清除上一次连接失败留下的告警。WebSocket 协议
    // 之前没有触发 on_connected_，导致一次瞬时失败后“无法连接服务”横幅
    // 即使后续连接成功也不会消失。
    if (on_connected_ != nullptr) {
        on_connected_();
    }
    if (on_audio_channel_opened_ != nullptr) {
        on_audio_channel_opened_();
    }

    // 主动心跳：20 秒一次 Ping，避免服务端/中间设备把长时间无上行数据的
    // WebSocket 当作空闲连接断开。Pong 会刷新 last_incoming_time_。
    MarkIncomingTime();
    if (ping_timer_ != nullptr) {
        esp_timer_stop(ping_timer_);
        esp_timer_start_periodic(ping_timer_, 20 * 1000 * 1000);
    }

    return true;
}

void WebsocketProtocol::PingTimerCallback() {
    if (websocket_ != nullptr && websocket_->IsConnected()) {
        websocket_->Ping();
    }
}

std::string WebsocketProtocol::GetHelloMessage() {
    // keys: message type, version, audio_params (format, sample_rate, channels)
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddNumberToObject(root, "version", version_);
    AddClientCapabilities(root);
    cJSON_AddStringToObject(root, "transport", "websocket");
    cJSON* audio_params = cJSON_CreateObject();
    cJSON_AddStringToObject(audio_params, "format", "opus");
    cJSON_AddNumberToObject(audio_params, "sample_rate", 16000);
    cJSON_AddNumberToObject(audio_params, "channels", 1);
    cJSON_AddNumberToObject(audio_params, "frame_duration", OPUS_FRAME_DURATION_MS);
    cJSON_AddItemToObject(root, "audio_params", audio_params);
    auto json_str = cJSON_PrintUnformatted(root);
    std::string message(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return message;
}

void WebsocketProtocol::ParseServerHello(const cJSON* root) {
    auto transport = cJSON_GetObjectItem(root, "transport");
    if (!cJSON_IsString(transport)) {
        ESP_LOGE(TAG, "Missing or non-string transport in server hello");
        return;
    }
    if (strcmp(transport->valuestring, "websocket") != 0) {
        ESP_LOGE(TAG, "Unsupported transport: %s", transport->valuestring);
        return;
    }

    auto session_id = cJSON_GetObjectItem(root, "session_id");
    if (cJSON_IsString(session_id)) {
        session_id_ = session_id->valuestring;
        ESP_LOGI(TAG, "Session ID: %s", session_id_.c_str());
    }

    auto audio_params = cJSON_GetObjectItem(root, "audio_params");
    if (cJSON_IsObject(audio_params)) {
        auto sample_rate = cJSON_GetObjectItem(audio_params, "sample_rate");
        if (cJSON_IsNumber(sample_rate)) {
            const int rate = sample_rate->valueint;
            if (rate == 8000 || rate == 16000 || rate == 24000 || rate == 48000) {
                server_sample_rate_ = rate;
            } else {
                ESP_LOGW(TAG, "Unsupported server sample_rate: %d, keep %d", rate, server_sample_rate_);
            }
        }
        auto frame_duration = cJSON_GetObjectItem(audio_params, "frame_duration");
        if (cJSON_IsNumber(frame_duration)) {
            const int duration = frame_duration->valueint;
            if (duration == 10 || duration == 20 || duration == 40 || duration == 60) {
                server_frame_duration_ = duration;
            } else {
                ESP_LOGW(TAG, "Unsupported server frame_duration: %d, keep %d", duration, server_frame_duration_);
            }
        }
    }

    xEventGroupSetBits(event_group_handle_, WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT);
}
