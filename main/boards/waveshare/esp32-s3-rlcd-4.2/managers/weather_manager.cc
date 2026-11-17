#include "weather_manager.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "zlib.h"
#include "../config.h"   // WEATHER_LAT / WEATHER_LON / WEATHER_CITY
#include <settings.h>
#if __has_include("weather_qweather_secret.h")
#include "weather_qweather_secret.h"
#endif
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
    // 编译期默认值：config.h（空）或 gitignore 的 weather_qweather_secret.h
    city_ = WEATHER_CITY;
#ifdef QWEATHER_HOST
    api_host_ = QWEATHER_HOST;
#endif
#ifdef QWEATHER_KEY
    api_key_ = QWEATHER_KEY;
#endif
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

// 通用 HTTPS GET：把响应体（自动处理 gzip）读进 out
bool WeatherManager::httpGetJson(const std::string& url, std::string& out, int timeout_ms) {
    if (!response_buffer) {
        return false;
    }
    response_len = 0;
    memset(response_buffer, 0, RESPONSE_BUFFER_SIZE);

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.event_handler = http_event_handler;
    config.timeout_ms = timeout_ms;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        return false;
    }
    esp_http_client_set_header(client, "User-Agent", "ESP32-RLCD-Weather");
    esp_http_client_set_header(client, "Accept-Encoding", "gzip");

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200 || response_len <= 0) {
        ESP_LOGW(TAG, "HTTP 请求失败 (err=%d, status=%d, len=%d) url=%.110s",
                 err, status, response_len, url.c_str());
        return false;
    }
    int d_len = 0;
    if (decompressed_buffer &&
        decompress_gzip_safe((uint8_t*)response_buffer, response_len,
                             decompressed_buffer, DECOMPRESSED_BUFFER_SIZE, &d_len)) {
        out.assign(decompressed_buffer, d_len);
    } else {
        response_buffer[response_len] = '\0';
        out.assign(response_buffer, response_len);
    }
    return true;
}

// URL 编码（城市名是中文，必须 percent-encode）
static std::string url_encode(const std::string& in) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(in.size() * 3);
    for (unsigned char c : in) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back((char)c);
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}

// "2026-11-17" -> "周二"
static std::string DateToWeek(const std::string& date) {
    int y = 0, m = 0, d = 0;
    if (sscanf(date.c_str(), "%d-%d-%d", &y, &m, &d) != 3 || m < 1 || m > 12) {
        return "";
    }
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) {
        y -= 1;
    }
    int w = (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
    static const char* names[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
    return names[(w < 0) ? 0 : w];
}

// 只保留数字："高温 17℃" -> "17"
static std::string DigitsOnly(const std::string& in) {
    std::string out;
    for (char c : in) {
        if (c >= '0' && c <= '9') {
            out.push_back(c);
        }
    }
    return out;
}

// 从 NVS 命名空间 "weather" 读运行时可改配置
void WeatherManager::loadConfigFromNvs() {
    Settings settings("weather", false);
    std::string host = settings.GetString("api_host");
    std::string key = settings.GetString("api_key");
    std::string city = settings.GetString("city");
    if (!host.empty() && !key.empty()) {
        api_host_ = host;
        api_key_ = key;
    }
    if (!city.empty()) {
        city_ = city;
        manual_city_ = true;   // NVS 里手动指定了城市，跳过 IP 定位
    }
    std::string loc_id = settings.GetString("loc_id");
    std::string loc_name = settings.GetString("loc_name");
    if (!loc_id.empty()) {
        loc_id_ = loc_id;
        loc_name_ = loc_name;
    }
    ESP_LOGI(TAG, "天气配置: 城市=%s, 和风=%s, LocationID=%s",
             city_.c_str(), hasQWeather() ? "已配置" : "未配置",
             loc_id_.empty() ? "(待查询)" : loc_id_.c_str());
}

// IP 定位：ip-api.com 免费接口（HTTP，45 次/分钟），返回出口 IP 所在城市与经纬度
bool WeatherManager::LocateByIp() {
    if (manual_city_) {
        ESP_LOGI(TAG, "城市为手动配置(%s)，跳过 IP 定位", city_.c_str());
        return true;
    }
    std::string body;
    if (!httpGetJson(WEATHER_IP_GEO_URL, body, 8000)) {
        ESP_LOGW(TAG, "IP 定位请求失败，沿用城市 %s", city_.c_str());
        return false;
    }
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        ESP_LOGW(TAG, "IP 定位响应解析失败，沿用城市 %s", city_.c_str());
        return false;
    }
    bool ok = false;
    cJSON* status = cJSON_GetObjectItem(root, "status");
    if (status && cJSON_IsString(status) && strcmp(status->valuestring, "success") == 0) {
        cJSON* city = cJSON_GetObjectItem(root, "city");
        cJSON* region = cJSON_GetObjectItem(root, "regionName");
        cJSON* lat = cJSON_GetObjectItem(root, "lat");
        cJSON* lon = cJSON_GetObjectItem(root, "lon");
        if (lat && cJSON_IsNumber(lat) && lon && cJSON_IsNumber(lon)) {
            ip_lat_ = static_cast<float>(lat->valuedouble);
            ip_lon_ = static_cast<float>(lon->valuedouble);
            has_ip_coord_ = true;
        }
        if (city && cJSON_IsString(city) && city->valuestring[0] != 0) {
            city_ = city->valuestring;
        }
        // 城市变了，之前缓存的 LocationID 必须作废
        loc_id_.clear();
        loc_name_.clear();
        Settings wr("weather", true);
        wr.SetString("ip_city", city_);
        char coord[40];
        snprintf(coord, sizeof(coord), "%.4f,%.4f", (double)ip_lon_, (double)ip_lat_);
        wr.SetString("ip_coord", coord);
        ESP_LOGI(TAG, "IP 定位成功: %s %s (%.4f, %.4f)",
                 (region && cJSON_IsString(region)) ? region->valuestring : "",
                 city_.c_str(), (double)ip_lat_, (double)ip_lon_);
        ok = true;
    } else {
        ESP_LOGW(TAG, "IP 定位失败(status=%s)，沿用城市 %s",
                 (status && cJSON_IsString(status)) ? status->valuestring : "?", city_.c_str());
    }
    cJSON_Delete(root);
    return ok;
}

// 手动指定城市：清掉 LocationID 缓存，下次 update() 按新城市重新解析
void WeatherManager::SetCity(const std::string& city) {
    if (city.empty()) {
        return;
    }
    city_ = city;
    loc_id_.clear();
    loc_name_.clear();
    has_ip_coord_ = false;
    manual_city_ = true;
    Settings wr("weather", true);
    wr.SetString("city", city_);
    wr.SetString("loc_id", "");
    wr.SetString("loc_name", "");
    ESP_LOGI(TAG, "天气城市已手动设置为 %s", city_.c_str());
}

// 和风 LocationID：NVS 缓存 > geo 城市名查询 > geo 经纬度反查 > 城市代码兜底
std::string WeatherManager::ResolveLocationId() {
    if (!loc_id_.empty()) {
        return loc_id_;
    }
    auto lookup = [&](const std::string& location_param) -> bool {
        std::string body;
        char url[448];
        snprintf(url, sizeof(url), "https://%s/geo/v2/city/lookup?location=%s&lang=zh&key=%s",
                 api_host_.c_str(), location_param.c_str(), api_key_.c_str());
        if (!httpGetJson(url, body, 12000)) {
            return false;
        }
        bool ok = false;
        cJSON* root = cJSON_Parse(body.c_str());
        if (root != nullptr) {
            cJSON* code = cJSON_GetObjectItem(root, "code");
            cJSON* locs = cJSON_GetObjectItem(root, "location");
            if (code && cJSON_IsString(code) && strcmp(code->valuestring, "200") == 0 &&
                locs && cJSON_GetArraySize(locs) > 0) {
                cJSON* first = cJSON_GetArrayItem(locs, 0);
                cJSON* id = cJSON_GetObjectItem(first, "id");
                cJSON* name = cJSON_GetObjectItem(first, "name");
                if (id && cJSON_IsString(id)) {
                    loc_id_ = id->valuestring;
                    if (name && cJSON_IsString(name)) {
                        loc_name_ = name->valuestring;
                    }
                    Settings wr("weather", true);
                    wr.SetString("loc_id", loc_id_);
                    wr.SetString("loc_name", loc_name_);
                    ESP_LOGI(TAG, "和风 geo 定位成功: %s (%s)", loc_name_.c_str(), loc_id_.c_str());
                    ok = true;
                }
            }
            cJSON_Delete(root);
        }
        return ok;
    };

    // 1) 城市名（IP 定位得到的城市名，或手动配置的城市名）
    if (!city_.empty() && lookup(url_encode(city_))) {
        return loc_id_;
    }
    // 2) 经纬度反查（城市名查不到时兜底，可能返回区县级名称）
    if (has_ip_coord_) {
        char coord[40];
        snprintf(coord, sizeof(coord), "%.4f,%.4f", (double)ip_lon_, (double)ip_lat_);
        if (lookup(coord)) {
            return loc_id_;
        }
    }
    loc_id_ = WEATHER_CITY_CODE;
    ESP_LOGW(TAG, "geo 定位失败，直接用城市代码 %s", loc_id_.c_str());
    return loc_id_;
}

// 和风天气：实时 + 空气质量 + 3 天预报 + 预警
bool WeatherManager::updateQWeather() {
    const std::string loc = ResolveLocationId();
    if (loc.empty()) {
        return false;
    }
    std::string body;
    char url[448];
    auto str_of = [](cJSON* obj, const char* k) -> std::string {
        cJSON* it = obj ? cJSON_GetObjectItem(obj, k) : nullptr;
        if (it == nullptr) {
            return "";
        }
        if (cJSON_IsString(it)) {
            return it->valuestring;
        }
        if (cJSON_IsNumber(it)) {
            char buf[24];
            snprintf(buf, sizeof(buf), "%d", it->valueint);
            return buf;
        }
        return "";
    };

    // 1) 实时天气
    snprintf(url, sizeof(url), "https://%s/v7/weather/now?location=%s&lang=zh&key=%s",
             api_host_.c_str(), loc.c_str(), api_key_.c_str());
    if (!httpGetJson(url, body, 15000)) {
        return false;
    }
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        ESP_LOGW(TAG, "和风实时天气解析失败 (head=%.80s)", body.c_str());
        return false;
    }
    cJSON* code = cJSON_GetObjectItem(root, "code");
    cJSON* now = cJSON_GetObjectItem(root, "now");
    if (!(code && cJSON_IsString(code) && strcmp(code->valuestring, "200") == 0 && now)) {
        ESP_LOGW(TAG, "和风实时天气返回异常 code=%s",
                 (code && cJSON_IsString(code)) ? code->valuestring : "null");
        cJSON_Delete(root);
        return false;
    }
    latest_data_.temp = str_of(now, "temp");
    latest_data_.text = str_of(now, "text");
    latest_data_.feels_like = str_of(now, "feelsLike");
    latest_data_.humidity = str_of(now, "humidity");
    latest_data_.wind_dir = str_of(now, "windDir");
    latest_data_.wind_scale = str_of(now, "windScale");
    latest_data_.vis = str_of(now, "vis");
    latest_data_.pressure = str_of(now, "pressure");
    std::string upd = str_of(root, "updateTime");
    latest_data_.update_time = upd.empty() ? "qweather" : upd;
    latest_data_.city = loc_name_.empty() ? city_ : loc_name_;
    cJSON_Delete(root);

    // 2) 空气质量
    snprintf(url, sizeof(url), "https://%s/v7/air/now?location=%s&lang=zh&key=%s",
             api_host_.c_str(), loc.c_str(), api_key_.c_str());
    if (httpGetJson(url, body, 12000)) {
        cJSON* aroot = cJSON_Parse(body.c_str());
        if (aroot) {
            cJSON* anow = cJSON_GetObjectItem(aroot, "now");
            if (anow) {
                latest_data_.aqi = str_of(anow, "aqi");
                latest_data_.aqi_category = str_of(anow, "category");
                latest_data_.pm25 = str_of(anow, "pm2p5");
            }
            cJSON_Delete(aroot);
        }
    }

    // 3) 3 天预报（含日出日落 / 最高最低 / 风）
    snprintf(url, sizeof(url), "https://%s/v7/weather/3d?location=%s&lang=zh&key=%s",
             api_host_.c_str(), loc.c_str(), api_key_.c_str());
    if (httpGetJson(url, body, 15000)) {
        cJSON* droot = cJSON_Parse(body.c_str());
        if (droot) {
            cJSON* daily = cJSON_GetObjectItem(droot, "daily");
            int n = daily ? cJSON_GetArraySize(daily) : 0;
            latest_data_.daily_count = (n > 3) ? 3 : n;
            for (int i = 0; i < latest_data_.daily_count; ++i) {
                cJSON* d = cJSON_GetArrayItem(daily, i);
                WeatherDaily& wd = latest_data_.daily[i];
                wd.date = str_of(d, "fxDate");
                wd.week = DateToWeek(wd.date);
                wd.text_day = str_of(d, "textDay");
                wd.text_night = str_of(d, "textNight");
                wd.temp_min = str_of(d, "tempMin");
                wd.temp_max = str_of(d, "tempMax");
                wd.wind_dir = str_of(d, "windDirDay");
                wd.wind_scale = str_of(d, "windScaleDay");
                wd.sunrise = str_of(d, "sunrise");
                wd.sunset = str_of(d, "sunset");
            }
            if (latest_data_.daily_count > 0) {
                latest_data_.sunrise = latest_data_.daily[0].sunrise;
                latest_data_.sunset = latest_data_.daily[0].sunset;
            }
            cJSON_Delete(droot);
        }
    }

    // 4) 预警（只取第一条：类型 + 颜色等级）
    latest_data_.warning.clear();
    snprintf(url, sizeof(url), "https://%s/v7/warning/now?location=%s&lang=zh&key=%s",
             api_host_.c_str(), loc.c_str(), api_key_.c_str());
    if (httpGetJson(url, body, 12000)) {
        cJSON* wroot = cJSON_Parse(body.c_str());
        if (wroot) {
            cJSON* arr = cJSON_GetObjectItem(wroot, "warning");
            if (arr && cJSON_GetArraySize(arr) > 0) {
                cJSON* w0 = cJSON_GetArrayItem(arr, 0);
                // 和风返回英文颜色名（Blue/Yellow/Orange/Red/White/Black），转成中文等级
                const std::string color = str_of(w0, "severityColor");
                const char* color_zh = "";
                if (color == "White")       color_zh = "白色";
                else if (color == "Blue")   color_zh = "蓝色";
                else if (color == "Yellow") color_zh = "黄色";
                else if (color == "Orange") color_zh = "橙色";
                else if (color == "Red")    color_zh = "红色";
                else if (color == "Black")  color_zh = "黑色";
                latest_data_.warning = str_of(w0, "typeName") + color_zh;
            }
            cJSON_Delete(wroot);
        }
    }

    latest_data_.valid = true;
    ESP_LOGI(TAG, "天气更新成功(和风): %s %s %s°C 体感%s 湿度%s%% %s%s级 AQI %s %s 日出%s 日落%s%s%s",
             latest_data_.city.c_str(), latest_data_.text.c_str(), latest_data_.temp.c_str(),
             latest_data_.feels_like.c_str(), latest_data_.humidity.c_str(),
             latest_data_.wind_dir.c_str(), latest_data_.wind_scale.c_str(),
             latest_data_.aqi.c_str(), latest_data_.aqi_category.c_str(),
             latest_data_.sunrise.c_str(), latest_data_.sunset.c_str(),
             latest_data_.warning.empty() ? "" : " 预警:",
             latest_data_.warning.c_str());
    return true;
}

bool WeatherManager::update() {
    if (!response_buffer) {
        ESP_LOGW(TAG, "天气缓冲区未分配");
        return false;
    }
    // 1) 优先和风天气（维度最全：实时 / 空气质量 / 3 天预报 / 预警）
    if (hasQWeather()) {
        // 每次开机只做一次 IP 定位，决定这次用哪个城市查天气
        if (!ip_located_) {
            ip_located_ = true;
            LocateByIp();
        }
        if (updateQWeather()) {
            return true;
        }
        ESP_LOGW(TAG, "和风天气更新失败，回退免密钥数据源");
    }
    // 2) 免密钥兜底：中国天气网 -> Open-Meteo
    if (updateItboy()) {
        return true;
    }
    return updateOpenMeteo();
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

    // 免密钥源也补齐部分维度：AQI / 湿度 / 风力 / 日出日落 / 3 天预报
    auto it_str = [](cJSON* obj, const char* k) -> std::string {
        cJSON* it = obj ? cJSON_GetObjectItem(obj, k) : nullptr;
        if (it == nullptr) {
            return "";
        }
        if (cJSON_IsString(it)) {
            return it->valuestring;
        }
        if (cJSON_IsNumber(it)) {
            char buf[24];
            snprintf(buf, sizeof(buf), "%d", it->valueint);
            return buf;
        }
        return "";
    };
    latest_data_.humidity = it_str(data, "shidu");
    latest_data_.aqi = it_str(today, "aqi");
    latest_data_.aqi_category = it_str(data, "quality");
    latest_data_.pm25 = it_str(data, "pm25");
    latest_data_.wind_dir = it_str(today, "fx");
    latest_data_.wind_scale = DigitsOnly(it_str(today, "fl"));
    latest_data_.sunrise = it_str(today, "sunrise");
    latest_data_.sunset = it_str(today, "sunset");
    latest_data_.warning.clear();
    latest_data_.feels_like.clear();
    latest_data_.vis.clear();
    latest_data_.pressure.clear();
    latest_data_.daily_count = 0;
    int dn = forecast ? cJSON_GetArraySize(forecast) : 0;
    for (int i = 0; i < dn && i < 3; ++i) {
        cJSON* d = cJSON_GetArrayItem(forecast, i);
        WeatherDaily& wd = latest_data_.daily[i];
        wd.date = it_str(d, "ymd");
        wd.week = it_str(d, "week");
        wd.text_day = it_str(d, "type");
        wd.text_night = it_str(d, "type");
        wd.temp_max = DigitsOnly(it_str(d, "high"));
        wd.temp_min = DigitsOnly(it_str(d, "low"));
        wd.wind_dir = it_str(d, "fx");
        wd.wind_scale = DigitsOnly(it_str(d, "fl"));
        wd.sunrise = it_str(d, "sunrise");
        wd.sunset = it_str(d, "sunset");
        latest_data_.daily_count = i + 1;
    }

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
