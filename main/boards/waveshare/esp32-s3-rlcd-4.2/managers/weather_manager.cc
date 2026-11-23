#include "weather_manager.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "zlib.h"
#include "../config.h"   // WEATHER_LAT / WEATHER_LON / WEATHER_CITY
#include "../date_utils.h"
#include <settings.h>
#if __has_include("weather_qweather_secret.h")
#include "weather_qweather_secret.h"
#endif
#include <string.h>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <vector>

static const char *TAG = "WeatherManager";

// HTTP 响应上下文：按需增长，不再受固定 8KB 缓冲限制（避免大响应被静默截断）
struct HttpCtx {
    std::string data;
    bool overflow = false;   // 超过上限时置位，避免异常大响应把堆吃满
};

esp_err_t WeatherManager::http_event_handler(esp_http_client_event_t *evt) {
    switch(evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (evt->user_data != nullptr && evt->data != nullptr && evt->data_len > 0) {
                static constexpr size_t kMaxHttpResponseBytes = 512 * 1024;
                auto* ctx = static_cast<HttpCtx*>(evt->user_data);
                if (ctx->data.size() + evt->data_len > kMaxHttpResponseBytes) {
                    ctx->overflow = true;
                    break;
                }
                ctx->data.append(static_cast<const char*>(evt->data), evt->data_len);
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

WeatherManager::WeatherManager() {
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
    static WeatherData d;   // 放静态区：WeatherData 含 27 个 std::string，栈帧会超 1536 限制
    d = WeatherData();
    if (city.empty() || weather_text.empty() || temperature.empty()) {
        ESP_LOGW(TAG, "外部天气数据无效：city/text/temp 不能为空");
        return false;
    }

    d.city = city;
    d.text = weather_text;
    d.temp = temperature;
    d.update_time = update_time.empty() ? "mcp" : update_time;
    d.valid = true;

    ESP_LOGI(TAG, "天气已由外部写入: %s %s %s°C",
             d.city.c_str(),
             d.text.c_str(),
             d.temp.c_str());
    {
        std::lock_guard<std::mutex> lk(mutex_);
        latest_data_ = d;
        latest_time_ = std::chrono::steady_clock::now();
    }
    return true;
}

// GZIP 解压到 std::string：按块增长，响应体多大都不会被固定缓冲截断
static bool gunzip_to_string(const std::string& in, std::string& out) {
    out.clear();   // 必须清空：out 是复用的响应缓冲，追加会拼上前一个接口的 JSON
    if (in.size() < 18 || (unsigned char)in[0] != 0x1f || (unsigned char)in[1] != 0x8b) return false;
    z_stream strm = {};
    strm.next_in = (Bytef*)in.data();
    strm.avail_in = (uInt)in.size();
    if (inflateInit2(&strm, 15 + 16) != Z_OK) return false;
    // 堆上分块缓冲：避免 static 缓冲被并发解压共享，也避免 2KB 栈帧触发
    // -Werror=frame-larger-than。
    std::vector<char> buf(2048);
    int ret = Z_OK;
    do {
        strm.next_out = (Bytef*)buf.data();
        strm.avail_out = (uInt)buf.size();
        ret = inflate(&strm, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END) { inflateEnd(&strm); return false; }
        out.append(buf.data(), buf.size() - strm.avail_out);
    } while (ret == Z_OK);   // 必须一直解到 Z_STREAM_END：压缩流可能先耗尽 avail_in 但仍有输出待取
    inflateEnd(&strm);
    return ret == Z_STREAM_END;
}

// 判断城市名是否可用于 UI 展示（排除 "Ip" 这类占位值）
static bool is_valid_display_city(const char* city) {
    if (city == nullptr || city[0] == '\0') {
        return false;
    }
    std::string normalized(city);
    normalized.erase(std::remove_if(normalized.begin(), normalized.end(),
                                    [](unsigned char c) { return std::isspace(c) != 0; }),
                     normalized.end());
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

// 通用 HTTP(S) GET：把响应体原始字节读入 out（dynamic，无固定上限）
bool WeatherManager::httpGetRaw(const std::string& url, int timeout_ms, bool verify_tls, std::string& out) {
    HttpCtx ctx;
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.event_handler = http_event_handler;
    config.user_data = &ctx;
    config.timeout_ms = timeout_ms;
    if (verify_tls) {
        config.crt_bundle_attach = esp_crt_bundle_attach;
    }

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        return false;
    }
    esp_http_client_set_header(client, "User-Agent", "ESP32-RLCD-Weather");
    esp_http_client_set_header(client, "Accept-Encoding", "gzip");

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200 || ctx.data.empty() || ctx.overflow) {
        ESP_LOGW(TAG, "HTTP 请求失败 (err=%d, status=%d, len=%d) url=%.110s",
                 err, status, (int)ctx.data.size(), url.c_str());
        return false;
    }
    out.swap(ctx.data);
    return true;
}

// 通用 HTTPS GET：成功时把响应体（自动处理 gzip）写入 out
bool WeatherManager::httpGetJson(const std::string& url, std::string& out, int timeout_ms) {
    std::string raw;
    if (!httpGetRaw(url, timeout_ms, true, raw)) {
        return false;
    }
    if (gunzip_to_string(raw, out)) {
        return true;
    }
    out.swap(raw);
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
    return std::string("周") + date_utils::WeekdayCharZh(w);
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
    std::string adm = settings.GetString("adm");
    if (!host.empty() && !key.empty()) {
        api_host_ = host;
        api_key_ = key;
    }
    if (!city.empty()) {
        city_ = city;
        adm_ = adm;
        manual_city_ = true;   // NVS 里手动指定了城市：最高优先级，跳过 IP 定位
    }
    std::string loc_id = settings.GetString("loc_id");
    std::string loc_name = settings.GetString("loc_name");
    if (!loc_id.empty()) {
        loc_id_ = loc_id;
        loc_name_ = loc_name;
    }
    // 后端地址缓存：后端是权威来源，开机先用上次结果，网络就绪后 LocateFromBackend() 刷新。
    backend_cache_city_ = settings.GetString("backend_city");
    backend_cache_adm_ = settings.GetString("backend_adm");
    backend_cache_loc_id_ = settings.GetString("backend_loc_id");
    if (!manual_city_ && !backend_cache_city_.empty()) {
        city_ = backend_cache_city_;
        adm_ = backend_cache_adm_;
        loc_id_.clear();
        loc_name_.clear();
        if (!backend_cache_loc_id_.empty()) {
            loc_id_ = backend_cache_loc_id_;
            loc_name_ = backend_cache_city_;
        }
    }
    // 上次 IP 定位结果只作「开机先用」的缓存值；有后端缓存时不再用 IP 缓存覆盖。
    ip_cache_city_ = settings.GetString("ip_city");
    ip_cache_region_ = settings.GetString("ip_region");
    if (!manual_city_ && backend_cache_city_.empty() && !ip_cache_city_.empty()) {
        city_ = ip_cache_city_;
        std::string coord = settings.GetString("ip_coord");
        float la = 0.0f, lo = 0.0f;
        if (sscanf(coord.c_str(), "%f,%f", &la, &lo) == 2) {
            ip_lat_ = la;
            ip_lon_ = lo;
            has_ip_coord_ = true;
        }
    }
    city_source_ = manual_city_ ? "手动配置"
                   : (!backend_cache_city_.empty() ? "后端缓存"
                      : (ip_cache_city_.empty() ? "默认" : "上次定位缓存"));
    // LocationID 必须与当前城市同源，否则会拿旧城市的 ID 查新城市（历史上南京/常州串过）。
    if (!loc_id_.empty() && !loc_name_.empty() && loc_name_ != city_) {
        loc_id_.clear();
        loc_name_.clear();
    }
    ESP_LOGI(TAG, "天气配置: 城市=%s(来源=%s, 上级=%s), 和风=%s, LocationID=%s",
             city_.c_str(), city_source_.c_str(), adm_.empty() ? "-" : adm_.c_str(),
             hasQWeather() ? "已配置" : "未配置",
             loc_id_.empty() ? "(待查询)" : loc_id_.c_str());
}

// 解析三类免费 IP 定位接口的响应（字段名各不相同），成功返回 true 并填充经纬度/城市/省份。
// 参考 xinnan-tech/xiaozhi-esp32-server 的 get_ip_info()：IP 只作初值，最终仍要用天气商
// geo 接口归一化成正确的地名（IP 库只到城市级，热点/CGNAT 下必然偏）。
static bool parse_ip_geo(const std::string& body, const char* source,
                         std::string& out_city, std::string& out_region,
                         float& out_lat, float& out_lon) {
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        ESP_LOGW(TAG, "IP 定位(%s)响应非 JSON", source);
        return false;
    }
    auto str_of = [&](const char* k) -> std::string {
        cJSON* it = cJSON_GetObjectItem(root, k);
        return (it && cJSON_IsString(it) && it->valuestring) ? std::string(it->valuestring) : std::string();
    };
    auto num_of = [&](const char* k, float& dst) -> bool {
        cJSON* it = cJSON_GetObjectItem(root, k);
        if (it && cJSON_IsNumber(it)) { dst = (float)it->valuedouble; return true; }
        if (it && cJSON_IsString(it) && it->valuestring[0]) { dst = (float)atof(it->valuestring); return true; }
        return false;
    };

    std::string status = str_of("status");
    std::string err = str_of("error");
    bool looks_ok = err.empty() && (status.empty() || status == "success");
    bool ok = false;
    if (looks_ok) {
        out_city = str_of("city");
        out_region = str_of("regionName");
        if (out_region.empty()) out_region = str_of("region");
        float la = 0.0f, lo = 0.0f;
        bool has_lat = num_of("lat", la) || num_of("latitude", la);
        bool has_lon = num_of("lon", lo) || num_of("longitude", lo);
        if (!(has_lat && has_lon)) {
            std::string loc = str_of("loc");   // ipinfo.io: "32.0607,118.7630"
            if (!loc.empty() && sscanf(loc.c_str(), "%f,%f", &la, &lo) == 2) {
                has_lat = has_lon = true;
            }
        }
        if (has_lat && has_lon) { out_lat = la; out_lon = lo; }
        ok = !out_city.empty();
        if (ok) {
            ESP_LOGI(TAG, "IP 定位(%s)成功: %s %s (%.4f, %.4f)", source,
                     out_region.c_str(), out_city.c_str(), (double)out_lat, (double)out_lon);
        }
    } else {
        ESP_LOGW(TAG, "IP 定位(%s)返回失败: status=%s error=%s", source,
                 status.c_str(), err.c_str());
    }
    cJSON_Delete(root);
    return ok;
}

// 后端地址：GET /xiaozhi/weather，后端返回 {code:0,data:{city,adm,location_id}}。
// 设备不再自己用出口 IP 判城市；后端位置由 config.yaml 的 device_location 配置。
bool WeatherManager::LocateFromBackend() {
    if (manual_city_) {
        return true;
    }
    std::string body;
    if (!httpGetJson(WEATHER_BACKEND_GEO_URL, body, 10000)) {
        ESP_LOGW(TAG, "后端天气地址请求失败");
        return false;
    }
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        ESP_LOGW(TAG, "后端天气地址响应非 JSON");
        return false;
    }
    cJSON* code = cJSON_GetObjectItem(root, "code");
    cJSON* data = cJSON_GetObjectItem(root, "data");
    bool code_ok = code && ((cJSON_IsNumber(code) && code->valueint == 0) ||
                            (cJSON_IsString(code) && strcmp(code->valuestring, "0") == 0));
    if (!code_ok || data == nullptr || !cJSON_IsObject(data)) {
        cJSON* msg = cJSON_GetObjectItem(root, "msg");
        ESP_LOGW(TAG, "后端天气地址返回失败: %s",
                 (msg && cJSON_IsString(msg)) ? msg->valuestring : "unknown");
        cJSON_Delete(root);
        return false;
    }
    auto str_of = [&](const char* k) -> std::string {
        cJSON* it = cJSON_GetObjectItem(data, k);
        return (it && cJSON_IsString(it) && it->valuestring) ? std::string(it->valuestring) : std::string();
    };
    std::string city = str_of("city");
    std::string adm = str_of("adm");
    std::string id = str_of("location_id");
    if (city.empty() || id.empty()) {
        ESP_LOGW(TAG, "后端天气地址缺少城市或 LocationID");
        cJSON_Delete(root);
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(loc_mutex_);
        city_ = city;
        adm_ = adm;
        loc_id_ = id;
        loc_name_ = city;
        has_ip_coord_ = false;   // 后端已给出 LocationID，禁止再用出口 IP 经纬度反查
        city_source_ = "后端服务";
    }
    Settings wr("weather", true);
    wr.SetString("backend_city", city);
    wr.SetString("backend_adm", adm);
    wr.SetString("backend_loc_id", id);
    ESP_LOGI(TAG, "后端天气地址: %s %s (%s)", adm.c_str(), city.c_str(), id.c_str());
    cJSON_Delete(root);
    return true;
}

// IP 定位：依次尝试 ip-api → ipapi.co → ipinfo.io（都是免密钥公开接口）。
// 仅在没有手动配置城市时使用；全部失败则回退上次定位缓存，再不行沿用当前城市。
bool WeatherManager::LocateByIp() {
    {
        std::lock_guard<std::mutex> lock(loc_mutex_);
        if (manual_city_) {
            ESP_LOGI(TAG, "城市为手动配置(%s)，跳过 IP 定位", city_.c_str());
            return true;
        }
    }
    const char* urls[3] = {WEATHER_IP_GEO_URL, WEATHER_IP_GEO_URL_ALT1, WEATHER_IP_GEO_URL_ALT2};
    const char* names[3] = {"ip-api", "ipapi.co", "ipinfo.io"};
    std::string city, region;
    float lat = 0.0f, lon = 0.0f;
    bool ok = false;
    for (int i = 0; i < 3 && !ok; i++) {
        std::string body;
        if (!httpGetJson(urls[i], body, 8000)) {
            ESP_LOGW(TAG, "IP 定位(%s)请求失败", names[i]);
            continue;
        }
        city.clear();
        region.clear();
        ok = parse_ip_geo(body, names[i], city, region, lat, lon);
    }
    if (!ok) {
        std::lock_guard<std::mutex> lock(loc_mutex_);
        if (!ip_cache_city_.empty()) {
            city_ = ip_cache_city_;
            city_source_ = "上次定位缓存";
            ESP_LOGW(TAG, "IP 定位全部失败，沿用上次定位缓存城市 %s", city_.c_str());
            return true;
        }
        ESP_LOGW(TAG, "IP 定位全部失败，沿用城市 %s", city_.c_str());
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(loc_mutex_);
        city_ = city;
        if (!region.empty()) {
            adm_ = region;   // 仅作 geo 消歧提示（英文省份会被自动忽略）
        }
        if (lat != 0.0f || lon != 0.0f) {
            ip_lat_ = lat;
            ip_lon_ = lon;
            has_ip_coord_ = true;
        }
        // 城市变了，之前缓存的 LocationID 必须作废
        loc_id_.clear();
        loc_name_.clear();
        city_source_ = "IP 定位";
    }
    Settings wr("weather", true);
    wr.SetString("ip_city", city);
    wr.SetString("ip_region", region);
    char coord[40];
    snprintf(coord, sizeof(coord), "%.4f,%.4f", (double)lon, (double)lat);
    wr.SetString("ip_coord", coord);
    return true;
}

// 手动指定城市：清掉 LocationID 缓存、禁用经纬度反查，下次 update() 按新城市重新解析
void WeatherManager::SetCity(const std::string& city, const std::string& adm) {
    if (city.empty()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(loc_mutex_);
        city_ = city;
        adm_ = adm;
        loc_id_.clear();
        loc_name_.clear();
        has_ip_coord_ = false;   // 手动城市优先，禁止用 IP 经纬度反查，避免被出口 IP 带偏
        manual_city_ = true;
        city_source_ = "手动配置";
    }
    Settings wr("weather", true);
    wr.SetString("city", city);
    wr.SetString("adm", adm);
    wr.SetString("loc_id", "");
    wr.SetString("loc_name", "");
    ESP_LOGI(TAG, "天气城市已手动设置为 %s(上级=%s)", city.c_str(), adm.empty() ? "-" : adm.c_str());
}

// 和风 LocationID：NVS 缓存 > 经纬度反查 > 城市名(+上级行政区划) > 城市代码兜底
std::string WeatherManager::ResolveLocationId() {
    std::string loc_id_snapshot, city_snapshot, adm_snapshot, city_source_snapshot;
    bool has_coord = false;
    float lat = 0.0f, lon = 0.0f;
    {
        std::lock_guard<std::mutex> lock(loc_mutex_);
        loc_id_snapshot = loc_id_;
        city_snapshot = city_;
        adm_snapshot = adm_;
        city_source_snapshot = city_source_;
        has_coord = has_ip_coord_;
        lat = ip_lat_;
        lon = ip_lon_;
    }
    if (!loc_id_snapshot.empty()) {
        return loc_id_snapshot;
    }
    std::string resolved_id;
    auto lookup = [&](const std::string& location_param, const std::string& adm_param) -> bool {
        std::string body;
        // std::string 拼接避免固定缓冲截断（中文 percent-encode 后长度不可控）
        std::string url = "https://" + api_host_ + "/geo/v2/city/lookup?location=" + location_param;
        if (!adm_param.empty()) {
            url += "&adm=" + adm_param;
        }
        url += "&lang=zh&key=" + api_key_;
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
                    std::string raw_name = (name && cJSON_IsString(name)) ? std::string(name->valuestring) : std::string();
                    // 优先取 adm2（地级市），避免把「玄武区」这类区县名显示到屏幕上
                    std::string display = pick_city_name_for_display(first, raw_name);
                    if (display.size() > 3 && display.compare(display.size() - 3, 3, "市") == 0) {
                        display.erase(display.size() - 3);   // 「南京市」→「南京」
                    }
                    resolved_id = id->valuestring;
                    {
                        std::lock_guard<std::mutex> lock(loc_mutex_);
                        loc_id_ = resolved_id;
                        loc_name_ = display;
                        cJSON* adm1 = cJSON_GetObjectItem(first, "adm1");
                        if (adm1 && cJSON_IsString(adm1) && adm1->valuestring[0] != 0) {
                            adm_ = adm1->valuestring;
                        }
                        // IP 定位源（如 ipinfo）可能返回英文城市名，用和风解析出的中文地名回填，保证显示/播报一致
                        if (city_source_ == "IP 定位" && !display.empty()) {
                            city_ = display;
                        }
                    }
                    Settings wr("weather", true);
                    wr.SetString("loc_id", resolved_id);
                    wr.SetString("loc_name", display);
                    ESP_LOGI(TAG, "和风 geo 定位成功: %s (%s)", display.c_str(), resolved_id.c_str());
                    ok = true;
                }
            }
            cJSON_Delete(root);
        }
        return ok;
    };

    // 1) 经纬度优先：和风按坐标反查最接近真实位置，比 IP 库给的城市名更可靠
    if (has_coord) {
        char coord[40];
        snprintf(coord, sizeof(coord), "%.4f,%.4f", (double)lon, (double)lat);
        if (lookup(coord, "")) {
            return resolved_id;
        }
    }
    // 2) 城市名；有中文上级行政区划时带上 adm 消歧（同名城市，如「朝阳」）
    if (!city_snapshot.empty()) {
        bool adm_is_chinese = false;
        for (size_t i = 0; i < adm_snapshot.size(); i++) {
            if ((unsigned char)adm_snapshot[i] >= 0x80) { adm_is_chinese = true; break; }
        }
        if (adm_is_chinese && lookup(url_encode(city_snapshot), url_encode(adm_snapshot))) {
            return resolved_id;
        }
        if (lookup(url_encode(city_snapshot), "")) {
            return resolved_id;
        }
    }
    // 只有默认城市才回退到固定城市代码；用户手动设了别的城市时不要偷偷查常州，
    // 返回空让上层回退免密钥天气源（按其自身城市参数查询）。
    std::lock_guard<std::mutex> lock(loc_mutex_);
    if (city_snapshot == WEATHER_CITY) {
        loc_id_ = WEATHER_CITY_CODE;
        ESP_LOGW(TAG, "geo 定位失败，默认城市回退城市代码 %s", loc_id_.c_str());
        return loc_id_;
    }
    ESP_LOGW(TAG, "geo 定位失败，城市 %s 不回退默认城市代码", city_snapshot.c_str());
    return std::string();
}

// 和风天气：实时 + 空气质量 + 3 天预报 + 预警
bool WeatherManager::updateQWeather() {
    static WeatherData d;   // 放静态区：WeatherData 含 27 个 std::string，栈帧会超 1536 限制
    d = WeatherData();
    const std::string loc = ResolveLocationId();
    if (loc.empty()) {
        return false;
    }
    std::string body;
    std::string url;
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
    url = "https://" + api_host_ + "/v7/weather/now?location=" + loc + "&lang=zh&key=" + api_key_;
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
    d.temp = str_of(now, "temp");
    d.text = str_of(now, "text");
    d.feels_like = str_of(now, "feelsLike");
    d.humidity = str_of(now, "humidity");
    d.wind_dir = str_of(now, "windDir");
    d.wind_scale = str_of(now, "windScale");
    d.vis = str_of(now, "vis");
    d.pressure = str_of(now, "pressure");
    std::string upd = str_of(root, "updateTime");
    d.update_time = upd.empty() ? "qweather" : upd;
    d.city = loc_name_.empty() ? city_ : loc_name_;
    cJSON_Delete(root);

    // 2) 空气质量
    //    先清空旧值：本子请求失败时不保留上一轮 aqi，避免新旧混显
    d.aqi.clear();
    d.aqi_category.clear();
    d.pm25.clear();
    url = "https://" + api_host_ + "/v7/air/now?location=" + loc + "&lang=zh&key=" + api_key_;
    if (httpGetJson(url, body, 12000)) {
        cJSON* aroot = cJSON_Parse(body.c_str());
        if (aroot) {
            cJSON* anow = cJSON_GetObjectItem(aroot, "now");
            if (anow) {
                d.aqi = str_of(anow, "aqi");
                d.aqi_category = str_of(anow, "category");
                d.pm25 = str_of(anow, "pm2p5");
            } else {
                cJSON* acode = cJSON_GetObjectItem(aroot, "code");
                ESP_LOGW(TAG, "和风空气质量无 now 字段 code=%s head=%.60s",
                         (acode && cJSON_IsString(acode)) ? acode->valuestring : "null", body.c_str());
            }
            cJSON_Delete(aroot);
        } else {
            ESP_LOGW(TAG, "和风空气质量解析失败 head=%.80s", body.c_str());
        }
    }

    // 3) 3 天预报（含日出日落 / 最高最低 / 风）
    //    先重置预报计数与日出日落：本子请求失败时不保留上一轮数据
    d.daily_count = 0;
    d.sunrise.clear();
    d.sunset.clear();
    url = "https://" + api_host_ + "/v7/weather/3d?location=" + loc + "&lang=zh&key=" + api_key_;
    if (httpGetJson(url, body, 15000)) {
        cJSON* droot = cJSON_Parse(body.c_str());
        if (droot) {
            cJSON* daily = cJSON_GetObjectItem(droot, "daily");
            int n = daily ? cJSON_GetArraySize(daily) : 0;
            d.daily_count = (n > 3) ? 3 : n;
            for (int i = 0; i < d.daily_count; ++i) {
                cJSON* day = cJSON_GetArrayItem(daily, i);
                WeatherDaily& wd = d.daily[i];
                wd.date = str_of(day, "fxDate");
                wd.week = DateToWeek(wd.date);
                wd.text_day = str_of(day, "textDay");
                wd.text_night = str_of(day, "textNight");
                wd.temp_min = str_of(day, "tempMin");
                wd.temp_max = str_of(day, "tempMax");
                wd.wind_dir = str_of(day, "windDirDay");
                wd.wind_scale = str_of(day, "windScaleDay");
                wd.sunrise = str_of(day, "sunrise");
                wd.sunset = str_of(day, "sunset");
            }
            if (d.daily_count > 0) {
                d.sunrise = d.daily[0].sunrise;
                d.sunset = d.daily[0].sunset;
            } else {
                cJSON* dcode = cJSON_GetObjectItem(droot, "code");
                ESP_LOGW(TAG, "和风 3 天预报无 daily code=%s head=%.60s",
                         (dcode && cJSON_IsString(dcode)) ? dcode->valuestring : "null", body.c_str());
            }
            cJSON_Delete(droot);
        } else {
            ESP_LOGW(TAG, "和风 3 天预报解析失败 head=%.80s", body.c_str());
        }
    }

    // 4) 预警（只取第一条：类型 + 颜色等级）
    d.warning.clear();
    url = "https://" + api_host_ + "/v7/warning/now?location=" + loc + "&lang=zh&key=" + api_key_;
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
                d.warning = str_of(w0, "typeName") + color_zh;
            }
            cJSON_Delete(wroot);
        }
    }

    d.valid = true;
    ESP_LOGI(TAG, "天气更新成功(和风): %s %s %s°C 体感%s 湿度%s%% %s%s级 AQI %s %s 日出%s 日落%s%s%s",
             d.city.c_str(), d.text.c_str(), d.temp.c_str(),
             d.feels_like.c_str(), d.humidity.c_str(),
             d.wind_dir.c_str(), d.wind_scale.c_str(),
             d.aqi.c_str(), d.aqi_category.c_str(),
             d.sunrise.c_str(), d.sunset.c_str(),
             d.warning.empty() ? "" : " 预警:",
             d.warning.c_str());
    {
        std::lock_guard<std::mutex> lk(mutex_);
        latest_data_ = d;
        latest_time_ = std::chrono::steady_clock::now();
    }
    return true;
}

bool WeatherManager::update() {
    // 1) 优先和风天气（维度最全：实时 / 空气质量 / 3 天预报 / 预警）
    if (hasQWeather()) {
        // 每次开机只解析一次地址：后端优先，失败回退后端缓存 / 配置默认城市 / IP 定位
        if (!ip_located_) {
            ip_located_ = true;
            bool manual = false;
            {
                std::lock_guard<std::mutex> lock(loc_mutex_);
                manual = manual_city_;
            }
            if (!manual && !LocateFromBackend()) {
                if (!backend_cache_city_.empty()) {
                    {
                        std::lock_guard<std::mutex> lock(loc_mutex_);
                        city_ = backend_cache_city_;
                        adm_ = backend_cache_adm_;
                        loc_id_.clear();
                        loc_name_.clear();
                        if (!backend_cache_loc_id_.empty()) {
                            loc_id_ = backend_cache_loc_id_;
                            loc_name_ = backend_cache_city_;
                        }
                        has_ip_coord_ = false;
                        city_source_ = "后端缓存";
                    }
                    ESP_LOGW(TAG, "后端地址不可用，沿用后端缓存城市 %s", backend_cache_city_.c_str());
                } else if (WEATHER_CITY[0] != '\0') {
                    // 后端不可用时用编译期配置的城市兜底，绝不回退到出口 IP（IP 常被判到邻市）
                    {
                        std::lock_guard<std::mutex> lock(loc_mutex_);
                        city_ = WEATHER_CITY;
                        loc_id_.clear();      // 必须清掉旧城市的 LocationID，否则仍按南京查天气
                        loc_name_.clear();
                        has_ip_coord_ = false;
                        city_source_ = "配置默认";
                    }
                    ESP_LOGW(TAG, "后端地址不可用，使用配置默认城市 %s", WEATHER_CITY);
                } else {
                    LocateByIp();
                }
            }
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
    static WeatherData d;   // 放静态区：WeatherData 含 27 个 std::string，栈帧会超 1536 限制
    d = WeatherData();
    // std::string 拼接避免固定缓冲截断
    std::string url = std::string("http://t.weather.itboy.net/api/weather/city/") + WEATHER_CITY_CODE;

    std::string body;
    if (!httpGetRaw(url, 8000, false, body)) {
        return false;
    }
    // httpGetRaw 统一带 Accept-Encoding: gzip；中国天气网是纯 HTTP，若对端返回
    // gzip 就必须在这里解压，否则 cJSON 解析的是压缩字节。
    std::string decoded;
    if (gunzip_to_string(body, decoded)) {
        body.swap(decoded);
    }

    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        ESP_LOGW(TAG, "中国天气网响应解析失败 (head=%.80s)", body.c_str());
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
        // "成都市" -> "成都"，避免卡片文案过长。
        // UTF-8 一个汉字 3 字节，不能用 6 字节当长度阈值（3 字中文=9 字节会被误判），
        // 这里只拦截异常长字符串，再单独去掉末尾的“市”。
        if (city.size() > 24) {
            city = WEATHER_CITY;
        } else if (city.size() >= 3 &&
                   city.compare(city.size() - 3, 3, "市") == 0) {
            city.erase(city.size() - 3);
        }
    }

    d.city = city;
    d.temp = wendu->valuestring;
    d.text = type->valuestring;
    d.update_time = "weather.com.cn";

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
    d.humidity = it_str(data, "shidu");
    d.aqi = it_str(today, "aqi");
    d.aqi_category = it_str(data, "quality");
    d.pm25 = it_str(data, "pm25");
    d.wind_dir = it_str(today, "fx");
    d.wind_scale = DigitsOnly(it_str(today, "fl"));
    d.sunrise = it_str(today, "sunrise");
    d.sunset = it_str(today, "sunset");
    d.warning.clear();
    d.feels_like.clear();
    d.vis.clear();
    d.pressure.clear();
    d.daily_count = 0;
    int dn = forecast ? cJSON_GetArraySize(forecast) : 0;
    for (int i = 0; i < dn && i < 3; ++i) {
        cJSON* day = cJSON_GetArrayItem(forecast, i);
        WeatherDaily& wd = d.daily[i];
        wd.date = it_str(day, "ymd");
        wd.week = it_str(day, "week");
        wd.text_day = it_str(day, "type");
        wd.text_night = it_str(day, "type");
        wd.temp_max = DigitsOnly(it_str(day, "high"));
        wd.temp_min = DigitsOnly(it_str(day, "low"));
        wd.wind_dir = it_str(day, "fx");
        wd.wind_scale = DigitsOnly(it_str(day, "fl"));
        wd.sunrise = it_str(day, "sunrise");
        wd.sunset = it_str(day, "sunset");
        d.daily_count = i + 1;
    }

    d.valid = true;

    ESP_LOGI(TAG, "天气更新成功(中国天气网): %s %s %s°C",
             d.city.c_str(), d.text.c_str(),
             d.temp.c_str());
    cJSON_Delete(root);
    {
        std::lock_guard<std::mutex> lk(mutex_);
        latest_data_ = d;
        latest_time_ = std::chrono::steady_clock::now();
    }
    return true;
}

// 免密钥天气源（备用）：Open-Meteo（无需注册、无 Key）
// 请求：当前温度 + WMO 天气代码；坐标由 config.h 的 WEATHER_LAT/WEATHER_LON 指定
bool WeatherManager::updateOpenMeteo() {
    static WeatherData d;   // 放静态区：WeatherData 含 27 个 std::string，栈帧会超 1536 限制
    d = WeatherData();
    // std::string 拼接避免固定缓冲截断；经纬度先格式化到小缓冲再拼
    char coords[80];
    snprintf(coords, sizeof(coords), "%.4f&longitude=%.4f", (double)WEATHER_LAT, (double)WEATHER_LON);
    std::string url = std::string("https://api.open-meteo.com/v1/forecast?latitude=") + coords +
                      "&current=temperature_2m,weather_code&timezone=Asia%2FShanghai";

    ESP_LOGI(TAG, "使用 Open-Meteo 获取天气 (%.4f, %.4f)", (double)WEATHER_LAT, (double)WEATHER_LON);

    std::string body;
    if (!httpGetRaw(url, 10000, true, body)) {
        return false;
    }
    std::string json;
    if (!gunzip_to_string(body, json)) {
        json.swap(body);
    }

    cJSON* root = cJSON_Parse(json.c_str());
    if (root == nullptr) {
        ESP_LOGW(TAG, "Open-Meteo 响应解析失败 (head=%.80s)", json.c_str());
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

    d.city = WEATHER_CITY;
    d.temp = temp_str;
    d.text = wmo_code_to_zh(code);
    d.update_time = "open-meteo";
    d.valid = true;

    ESP_LOGI(TAG, "天气更新成功(Open-Meteo): %s %s %s°C (code=%d)",
             d.city.c_str(), d.text.c_str(),
             d.temp.c_str(), code);
    cJSON_Delete(root);
    {
        std::lock_guard<std::mutex> lk(mutex_);
        latest_data_ = d;
        latest_time_ = std::chrono::steady_clock::now();
    }
    return true;
}
