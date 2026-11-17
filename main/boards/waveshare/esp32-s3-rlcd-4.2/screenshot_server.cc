// 屏幕截图 HTTP 服务
//
// 目的：RLCD 是 1-bit 单色屏，没法用摄像头/系统截图工具看画面，所以把显存导出成 BMP
// 走局域网 HTTP，主机侧 curl 一下就能看到设备真实画面（也方便回归对比 UI 改动）。
//
// 端点：GET /shot.bmp  → image/bmp，1bpp 黑白，尺寸 = 面板宽高
// 端口：8080（避开配网页面的 80 端口）

#include "screenshot_server.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_http_server.h>
#include <esp_log.h>

#include "custom_lcd_display.h"

static const char *TAG = "Screenshot";

static httpd_handle_t s_server = nullptr;
static CustomLcdDisplay *s_display = nullptr;
static uint8_t *s_bmp = nullptr;
static size_t s_bmp_cap = 0;
static const int kShotPort = 8080;

static esp_err_t ShotBmpHandler(httpd_req_t *req) {
    if (s_display == nullptr || s_bmp == nullptr) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "not ready");
    }
    size_t len = 0;
    if (!s_display->CaptureScreenBmp(s_bmp, s_bmp_cap, &len)) {
        ESP_LOGE(TAG, "截图失败");
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "capture failed");
    }
    httpd_resp_set_type(req, "image/bmp");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, (const char *)s_bmp, (ssize_t)len);
    ESP_LOGI(TAG, "已发送截图 %u 字节", (unsigned)len);
    return err;
}

// 调试端点：把关键控件状态以文本返回，排查「控件不显示」这类问题
static esp_err_t DbgHandler(httpd_req_t *req) {
    if (s_display == nullptr) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "not ready");
    }
    char head[64];
    int hn = snprintf(head, sizeof(head), "dither_mode=%d thr=%d\n",
                      s_display->GetDitherMode(), s_display->GetThreshold());
    std::string info = std::string(head, (size_t)(hn > 0 ? hn : 0)) + s_display->GetUiDebugInfo();
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, info.c_str(), (ssize_t)info.size());
}

// 在线切换 1-bit 抖动模式：/dither?mode=0|1|2（不带参数只报告当前值），便于 A/B 对比观感
static esp_err_t DitherHandler(httpd_req_t *req) {
    if (s_display == nullptr) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "not ready");
    }
    char query[64] = {0};
    char value[8] = {0};
    int mode = -1;
    int thr = -1;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (httpd_query_key_value(query, "mode", value, sizeof(value)) == ESP_OK) {
            mode = atoi(value);
        }
        if (httpd_query_key_value(query, "thr", value, sizeof(value)) == ESP_OK) {
            thr = atoi(value);
        }
    }
    if (mode >= 0 && mode <= 4) {
        s_display->SetDitherMode(mode);
        s_display->RedrawAll();
    }
    if (thr >= 16 && thr <= 240) {
        s_display->SetThreshold(thr);
        s_display->RedrawAll();
    }
    char out[128];
    int n = snprintf(out, sizeof(out),
                     "dither_mode=%d thr=%d (0=threshold 1=bayer4 2=bayer8 3/4=band-dither)\n",
                     s_display->GetDitherMode(), s_display->GetThreshold());
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, out, n);
}

// 在线切页：/page?p=weather|music|toggle（不带参数只报告当前页），便于远程看另一页的画面
static esp_err_t PageHandler(httpd_req_t *req) {
    if (s_display == nullptr) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "not ready");
    }
    char query[64] = {0};
    char value[16] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "p", value, sizeof(value)) == ESP_OK) {
        if (strcmp(value, "music") == 0) {
            s_display->SwitchToMusicPage();
        } else if (strcmp(value, "weather") == 0) {
            s_display->SwitchToWeatherPage();
        } else if (strcmp(value, "toggle") == 0) {
            if (s_display->IsMusicMode()) {
                s_display->SwitchToWeatherPage();
            } else {
                s_display->SwitchToMusicPage();
            }
        }
    }
    const char *page = s_display->IsMusicMode() ? "music" : "weather";
    char out[64];
    int n = snprintf(out, sizeof(out), "page=%s\n", page);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, out, n);
}

static const httpd_uri_t kShotUri = {
    .uri = "/shot.bmp",
    .method = HTTP_GET,
    .handler = ShotBmpHandler,
    .user_ctx = nullptr,
};

static const httpd_uri_t kDbgUri = {
    .uri = "/dbg",
    .method = HTTP_GET,
    .handler = DbgHandler,
    .user_ctx = nullptr,
};

static const httpd_uri_t kPageUri = {
    .uri = "/page",
    .method = HTTP_GET,
    .handler = PageHandler,
    .user_ctx = nullptr,
};

static const httpd_uri_t kDitherUri = {
    .uri = "/dither",
    .method = HTTP_GET,
    .handler = DitherHandler,
    .user_ctx = nullptr,
};

bool ScreenshotServerStart(CustomLcdDisplay *display) {
    if (s_server != nullptr) {
        return true;
    }
    if (display == nullptr) {
        return false;
    }
    RlcdDriver *drv = display->rlcd();
    const int w = (drv != nullptr) ? drv->width() : 400;
    const int h = (drv != nullptr) ? drv->height() : 300;
    s_bmp_cap = 62 + (size_t)(((w + 31) / 32) * 4) * (size_t)h;
    s_bmp = (uint8_t *)heap_caps_malloc(s_bmp_cap, MALLOC_CAP_SPIRAM);
    if (s_bmp == nullptr) {
        ESP_LOGE(TAG, "截图缓冲区分配失败 (%u 字节)", (unsigned)s_bmp_cap);
        return false;
    }
    s_display = display;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = kShotPort;
    config.lru_purge_enable = true;
    config.stack_size = 8192;          // 截图逐像素遍历在同一线程内完成
    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP 服务启动失败（端口 %d 被占用？）", kShotPort);
        s_server = nullptr;
        heap_caps_free(s_bmp);
        s_bmp = nullptr;
        return false;
    }
    httpd_register_uri_handler(s_server, &kShotUri);
    httpd_register_uri_handler(s_server, &kDbgUri);
    httpd_register_uri_handler(s_server, &kDitherUri);
    httpd_register_uri_handler(s_server, &kPageUri);
    ESP_LOGI(TAG, "截图服务已启动: http://<设备IP>:%d/shot.bmp", kShotPort);
    return true;
}
