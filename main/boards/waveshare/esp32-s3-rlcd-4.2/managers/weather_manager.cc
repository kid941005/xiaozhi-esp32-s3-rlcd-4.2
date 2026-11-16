#include "weather_manager.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "zlib.h"
#include "../config.h"   // WEATHER_LAT / WEATHER_LON / WEATHER_CITY
#include <string.h>
#include <algorithm>
#include <cctype>

static const char *TAG = "WeatherManager";

// HTTP 响应缓冲区（分配在 SPIRAM 上，避免占用宝贵的内部 RAM）
static char* response_buffer = NULL;
static int response_len = 0;
static const int RESPONSE_BUFFER_SIZE = 8192;

// GZIP 解压缓冲区（和风天气 API 默认返回 gzip 压缩数据）
static char* decompressed_buffer = NULL;
static const int DECOMPRESSED_BUFFER_SIZE = 8192;

esp_err_t WeatherManager::http_event_handler(esp_http_client_event_t *evt) {
    switch(evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (response_buffer && response_len + evt->data_len < RESPONSE_BUFFER_SIZE - 1) {
                memcpy(response_buffer + response_len, evt->data, evt->data_len);
                response_len += evt->data_len;
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

WeatherManager::WeatherManager() {
    response_buffer = (char*)heap_caps_malloc(RESPONSE_BUFFER_SIZE, MALLOC_CAP_SPIRAM);
    decompressed_buffer = (char*)heap_caps_malloc(DECOMPRESSED_BUFFER_SIZE, MALLOC_CAP_SPIRAM);
}

WeatherManager& WeatherManager::getInstance() {
    static WeatherManager instance;
    return instance;
}

void WeatherManager::setApiConfig(const char* key, const char* host) {
    api_key_ = key;
    api_host_ = host;
}

bool WeatherManager::updateFromExternal(const std::string& city,
                                        const std::string& weather_text,
                                        const std::string& temperature,
                                        const std::string& update_time) {
    if (city.empty() || weather_text.empty() || temperature.empty()) {
        ESP_LOGW(TAG, "外部天气数据无效：city/text/temp 不能为空");
        return false;
    }

    latest_data_.city = city;
    latest_data_.text = weather_text;
    latest_data_.temp = temperature;
    latest_data_.update_time = update_time.empty() ? "mcp" : update_time;
    latest_data_.valid = true;

    ESP_LOGI(TAG, "天气已由外部写入: %s %s %s°C",
             latest_data_.city.c_str(),
             latest_data_.text.c_str(),
             latest_data_.temp.c_str());
    return true;
}

// GZIP 安全解压（和风天气 API 返回 gzip 格式）
static bool decompress_gzip_safe(const uint8_t* src, int src_len, char* dst, int dst_max_len, int* out_len) {
    if (src_len < 18 || src[0] != 0x1f || src[1] != 0x8b) return false;
    z_stream strm = {};
    strm.next_in = (Bytef*)src;
    strm.avail_in = src_len;
    strm.next_out = (Bytef*)dst;
    strm.avail_out = dst_max_len - 1;
    if (inflateInit2(&strm, 15 + 16) != Z_OK) return false;
    int ret = inflate(&strm, Z_FINISH);
    inflateEnd(&strm);
    if (ret != Z_STREAM_END && ret != Z_OK) return false;
    *out_len = dst_max_len - 1 - strm.avail_out;
    dst[*out_len] = '\0';
    return true;
}

// 判断城市名是否可用于 UI 展示（排除 "Ip" 这类占位值）
static bool is_valid_display_city(const char* city) {
    if (city == nullptr || city[0] == '\0') {
        return false;
    }
    std::string normalized(city);
    normalized.erase(std::remove_if(normalized.begin(), normalized.end(), ::isspace), normalized.end());
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return normalized != "ip" && normalized != "auto_ip" && normalized != "unknown";
}

// 优先使用更像“真实地名”的字段，避免把 "Ip" 显示到页面上
static std::string pick_city_name_for_display(cJSON* first_city, const std::string& fallback_city) {
    const char* keys[] = {"adm2", "adm1", "name"};
    for (const char* key : keys) {
        cJSON* item = cJSON_GetObjectItem(first_city, key);
        if (item && cJSON_IsString(item) && is_valid_display_city(item->valuestring)) {
            return item->valuestring;
        }
    }
    return fallback_city;
}

bool WeatherManager::update() {
    if (!response_buffer) {
        ESP_LOGW(TAG, "天气缓冲区未分配");
        return false;
    }
    // 未配置和风天气 Key 时，退回免密钥数据源：先中国天气网，失败再试 Open-Meteo
    if (api_key_.empty() || api_host_.empty()) {
        if (updateItboy()) {
            return true;
        }
        return updateOpenMeteo();
    }

    // 第一步：通过和风天气 GeoAPI 进行 IP 定位
    response_len = 0;
    memset(response_buffer, 0, RESPONSE_BUFFER_SIZE);
    char geo_url[256];
    // 使用 auto_ip 让服务端按公网出口 IP 识别城市
    snprintf(geo_url, sizeof(geo_url), "https://%s/geo/v2/city/lookup?location=auto_ip&key=%s",
             api_host_.c_str(), api_key_.c_str());

    ESP_LOGI(TAG, "正在进行 IP 定位...");
    esp_http_client_config_t geo_config = {};
    geo_config.url = geo_url;
    geo_config.event_handler = http_event_handler;
    geo_config.timeout_ms = 8000;
    geo_config.crt_bundle_attach = esp_crt_bundle_attach;
    
    esp_http_client_handle_t geo_client = esp_http_client_init(&geo_config);
    esp_http_client_set_header(geo_client, "Host", api_host_.c_str());
    esp_err_t geo_err = esp_http_client_perform(geo_client);
    int geo_status = esp_http_client_get_status_code(geo_client);
    
    // 默认位置（苏州）
    double lat = 31.23, lon = 120.62; 
    std::string city_name = "苏州";

    if (geo_err == ESP_OK && geo_status == 200 && response_len > 0) {
        // geo 响应也可能是 gzip 压缩的，先尝试解压
        const char* geo_json = NULL;
        int geo_d_len = 0;
        if (decompressed_buffer &&
            decompress_gzip_safe((uint8_t*)response_buffer, response_len,
                                 decompressed_buffer, DECOMPRESSED_BUFFER_SIZE, &geo_d_len)) {
            geo_json = decompressed_buffer;
            ESP_LOGI(TAG, "IP 定位响应已 gzip 解压 (%d -> %d bytes)", response_len, geo_d_len);
        } else {
            response_buffer[response_len] = '\0';
            geo_json = response_buffer;
        }
        cJSON *root = cJSON_Parse(geo_json);
        if (root) {
            cJSON *code = cJSON_GetObjectItem(root, "code");
            if (code && cJSON_IsString(code) && strcmp(code->valuestring, "200") == 0) {
                cJSON *location_array = cJSON_GetObjectItem(root, "location");
                if (location_array && cJSON_GetArraySize(location_array) > 0) {
                    cJSON *first_city = cJSON_GetArrayItem(location_array, 0);
                    cJSON *lat_item = cJSON_GetObjectItem(first_city, "lat");
                    cJSON *lon_item = cJSON_GetObjectItem(first_city, "lon");
                    if (lat_item && lon_item &&
                        cJSON_IsString(lat_item) && cJSON_IsString(lon_item)) {
                        lat = atof(lat_item->valuestring);
                        lon = atof(lon_item->valuestring);
                        city_name = pick_city_name_for_display(first_city, city_name);
                        ESP_LOGI(TAG, "定位成功: %s (%.2f, %.2f)", city_name.c_str(), lat, lon);
                    } else {
                        ESP_LOGW(TAG, "定位响应缺少必要字段（lat/lon），使用默认城市");
                    }
                }
            } else {
                const char *api_code = (code && cJSON_IsString(code)) ? code->valuestring : "null";
                ESP_LOGW(TAG, "IP 定位接口返回 code=%s，使用默认城市", api_code);
            }
            cJSON_Delete(root);
        } else {
            // 打印响应前 80 字节帮助诊断（可能是 HTML 网关页、乱码等）
            ESP_LOGW(TAG, "IP 定位响应 JSON 解析失败 (len=%d, head=%.80s)，使用默认城市",
                     response_len, geo_json);
        }
    } else {
        // 打印失败细节，便于区分网络错误 / HTTP 错误 / 参数错误
        if (response_len > 0) {
            response_buffer[response_len] = '\0';
            ESP_LOGW(TAG, "IP 定位请求失败 (err=%d, status=%d, body=%.120s)，使用默认城市",
                     geo_err, geo_status, response_buffer);
        } else {
            ESP_LOGW(TAG, "IP 定位请求失败 (err=%d, status=%d, empty body)，使用默认城市",
                     geo_err, geo_status);
        }
    }
    esp_http_client_cleanup(geo_client);

    // 第二步：获取实时天气数据
    response_len = 0;
    memset(response_buffer, 0, RESPONSE_BUFFER_SIZE);
    char weather_url[512];
    snprintf(weather_url, sizeof(weather_url), 
             "https://%s/v7/weather/now?location=%.2f,%.2f&key=%s&lang=zh", 
             api_host_.c_str(), lon, lat, api_key_.c_str());

    ESP_LOGI(TAG, "获取天气数据...");
    esp_http_client_config_t weather_config = {};
    weather_config.url = weather_url;
    weather_config.event_handler = http_event_handler;
    weather_config.timeout_ms = 15000;
    weather_config.crt_bundle_attach = esp_crt_bundle_attach;
    esp_http_client_handle_t client = esp_http_client_init(&weather_config);
    
    esp_http_client_set_header(client, "Host", api_host_.c_str());
    esp_http_client_set_header(client, "User-Agent", "ESP32-Weather-Station");
    esp_http_client_set_header(client, "Accept-Encoding", "gzip");
    
    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    bool success = false;

    if (err == ESP_OK && status_code == 200 && response_len > 0) {
        int d_len = 0;
        const char* final_json = NULL;
        if (decompress_gzip_safe((uint8_t*)response_buffer, response_len, 
                                  decompressed_buffer, DECOMPRESSED_BUFFER_SIZE, &d_len)) {
            final_json = decompressed_buffer;
        } else {
            response_buffer[response_len] = '\0';
            final_json = response_buffer;
        }

        if (final_json) {
            parseWeatherJson(final_json);
            latest_data_.city = city_name;
            success = latest_data_.valid;
        }
    } else {
        ESP_LOGE(TAG, "天气请求失败 (err=%d, status=%d)", err, status_code);
    }
    esp_http_client_cleanup(client);
    return success;
}

// WMO 天气代码 → 中文描述（Open-Meteo 使用的编码）
static const char* wmo_code_to_zh(int code) {
    switch (code) {
        case 0:  return "晴";
        case 1:  return "晴间多云";
        case 2:  return "多云";
        case 3:  return "阴";
        case 45: case 48: return "雾";
        case 51: case 53: case 55: return "毛毛雨";
        case 56: case 57: case 66: case 67: return "冻雨";
        case 61: return "小雨";
        case 63: return "中雨";
        case 65: return "大雨";
        case 71: return "小雪";
        case 73: return "中雪";
        case 75: return "大雪";
        case 77: return "雪粒";
        case 80: return "阵雨";
        case 81: return "强阵雨";
        case 82: return "暴雨";
        case 85: case 86: return "阵雪";
        case 95: return "雷阵雨";
        case 96: case 99: return "雷阵雨伴冰雹";
        default: return "未知";
    }
}

// 免密钥天气源（主）：中国天气网数据接口（纯 HTTP，国内直连快，无需 Key）
// 返回结构：{"cityInfo":{"city":"成都市"},"data":{"wendu":"12.8","forecast":[{"type":"小雨",...}]}}
bool WeatherManager::updateItboy() {
    if (!response_buffer) {
        return false;
    }
    response_len = 0;
    memset(response_buffer, 0, RESPONSE_BUFFER_SIZE);

    char url[192];
    snprintf(url, sizeof(url), "http://t.weather.itboy.net/api/weather/city/%s", WEATHER_CITY_CODE);

    esp_http_client_config_t config = {};
    config.url = url;
    config.event_handler = http_event_handler;
    config.timeout_ms = 8000;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        return false;
    }
    esp_http_client_set_header(client, "User-Agent", "ESP32-RLCD-Weather");

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200 || response_len <= 0) {
        ESP_LOGW(TAG, "中国天气网请求失败 (err=%d, status=%d, len=%d)", err, status, response_len);
        return false;
    }
    response_buffer[response_len] = 0;

    cJSON* root = cJSON_Parse(response_buffer);
    if (root == nullptr) {
        ESP_LOGW(TAG, "中国天气网响应解析失败 (head=%.80s)", response_buffer);
        return false;
    }

    cJSON* data = cJSON_GetObjectItem(root, "data");
    cJSON* wendu = data ? cJSON_GetObjectItem(data, "wendu") : nullptr;
    cJSON* forecast = data ? cJSON_GetObjectItem(data, "forecast") : nullptr;
    cJSON* today = (forecast && cJSON_GetArraySize(forecast) > 0) ? cJSON_GetArrayItem(forecast, 0) : nullptr;
    cJSON* type = today ? cJSON_GetObjectItem(today, "type") : nullptr;

    if (wendu == nullptr || !cJSON_IsString(wendu) || type == nullptr || !cJSON_IsString(type)) {
        ESP_LOGW(TAG, "中国天气网响应缺少 wendu/type 字段");
        cJSON_Delete(root);
        return false;
    }

    std::string city = WEATHER_CITY;
    cJSON* city_info = cJSON_GetObjectItem(root, "cityInfo");
    cJSON* city_name = city_info ? cJSON_GetObjectItem(city_info, "city") : nullptr;
    if (city_name && cJSON_IsString(city_name) && city_name->valuestring[0] != 0) {
        city = city_name->valuestring;
        // "成都市" -> "成都"，避免卡片文案过长
        if (city.size() > 6) {
            city = WEATHER_CITY;
        } else if (city.size() >= 3 &&
                   city.compare(city.size() - 3, 3, "市") == 0) {
            city.erase(city.size() - 3);
        }
    }

    latest_data_.city = city;
    latest_data_.temp = wendu->valuestring;
    latest_data_.text = type->valuestring;
    latest_data_.update_time = "weather.com.cn";
    latest_data_.valid = true;

    ESP_LOGI(TAG, "天气更新成功(中国天气网): %s %s %s°C",
             latest_data_.city.c_str(), latest_data_.text.c_str(),
             latest_data_.temp.c_str());
    cJSON_Delete(root);
    return true;
}

// 免密钥天气源（备用）：Open-Meteo（无需注册、无 Key）
// 请求：当前温度 + WMO 天气代码；坐标由 config.h 的 WEATHER_LAT/WEATHER_LON 指定
bool WeatherManager::updateOpenMeteo() {
    if (!response_buffer) {
        return false;
    }
    response_len = 0;
    memset(response_buffer, 0, RESPONSE_BUFFER_SIZE);

    char url[320];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
             "&current=temperature_2m,weather_code&timezone=Asia%%2FShanghai",
             (double)WEATHER_LAT, (double)WEATHER_LON);

    ESP_LOGI(TAG, "使用 Open-Meteo 获取天气 (%.4f, %.4f)", (double)WEATHER_LAT, (double)WEATHER_LON);

    esp_http_client_config_t config = {};
    config.url = url;
    config.event_handler = http_event_handler;
    config.timeout_ms = 10000;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        ESP_LOGE(TAG, "Open-Meteo 客户端创建失败");
        return false;
    }
    esp_http_client_set_header(client, "User-Agent", "ESP32-RLCD-Weather");
    esp_http_client_set_header(client, "Accept-Encoding", "gzip");

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200 || response_len <= 0) {
        ESP_LOGW(TAG, "Open-Meteo 请求失败 (err=%d, status=%d, len=%d)", err, status, response_len);
        return false;
    }

    const char* json = nullptr;
    int d_len = 0;
    if (decompressed_buffer &&
        decompress_gzip_safe((uint8_t*)response_buffer, response_len,
                             decompressed_buffer, DECOMPRESSED_BUFFER_SIZE, &d_len)) {
        json = decompressed_buffer;
    } else {
        response_buffer[response_len] = '\0';
        json = response_buffer;
    }

    cJSON* root = cJSON_Parse(json);
    if (root == nullptr) {
        ESP_LOGW(TAG, "Open-Meteo 响应解析失败 (head=%.80s)", json);
        return false;
    }

    cJSON* current = cJSON_GetObjectItem(root, "current");
    cJSON* temp = current ? cJSON_GetObjectItem(current, "temperature_2m") : nullptr;
    cJSON* wcode = current ? cJSON_GetObjectItem(current, "weather_code") : nullptr;
    if (temp == nullptr || !cJSON_IsNumber(temp)) {
        ESP_LOGW(TAG, "Open-Meteo 响应缺少 current.temperature_2m");
        cJSON_Delete(root);
        return false;
    }

    const int code = (wcode && cJSON_IsNumber(wcode)) ? wcode->valueint : -1;
    char temp_str[16];
    snprintf(temp_str, sizeof(temp_str), "%.0f", temp->valuedouble);

    latest_data_.city = WEATHER_CITY;
    latest_data_.temp = temp_str;
    latest_data_.text = wmo_code_to_zh(code);
    latest_data_.update_time = "open-meteo";
    latest_data_.valid = true;

    ESP_LOGI(TAG, "天气更新成功(Open-Meteo): %s %s %s°C (code=%d)",
             latest_data_.city.c_str(), latest_data_.text.c_str(),
             latest_data_.temp.c_str(), code);
    cJSON_Delete(root);
    return true;
}

void WeatherManager::parseWeatherJson(const char* json_data) {
    cJSON *root = cJSON_Parse(json_data);
    if (!root) return;
    cJSON *code = cJSON_GetObjectItem(root, "code");
    if (code && strcmp(code->valuestring, "200") == 0) {
        cJSON *now = cJSON_GetObjectItem(root, "now");
        if (now) {
            latest_data_.temp = cJSON_GetObjectItem(now, "temp")->valuestring;
            latest_data_.text = cJSON_GetObjectItem(now, "text")->valuestring;
            latest_data_.valid = true;
            ESP_LOGI(TAG, "天气更新成功: %s°C, %s", latest_data_.temp.c_str(), latest_data_.text.c_str());
        }
    }
    cJSON_Delete(root);
}
