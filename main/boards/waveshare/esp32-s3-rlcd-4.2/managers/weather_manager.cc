#include "weather_manager.h"

#include <cJSON.h>
#include <esp_log.h>
#include <settings.h>

#include <cmath>
#include <cstdio>
#include <cstring>

static const char *TAG = "WeatherManager";

namespace {

std::string JsonString(cJSON* obj, const char* key) {
    cJSON* it = obj ? cJSON_GetObjectItem(obj, key) : nullptr;
    if (it != nullptr && cJSON_IsString(it) && it->valuestring != nullptr) {
        return it->valuestring;
    }
    return "";
}

// 数值转显示字符串；reminder get_weather 的温度/气压等是 JSON number。
std::string JsonNumber(cJSON* obj, const char* key, int decimals = 0) {
    cJSON* it = obj ? cJSON_GetObjectItem(obj, key) : nullptr;
    if (it == nullptr) {
        return "";
    }
    if (cJSON_IsString(it) && it->valuestring != nullptr) {
        return it->valuestring;
    }
    if (!cJSON_IsNumber(it)) {
        return "";
    }
    char buf[32];
    if (decimals > 0) {
        std::snprintf(buf, sizeof(buf), "%.1f", it->valuedouble);
    } else {
        std::snprintf(buf, sizeof(buf), "%.0f", it->valuedouble);
    }
    return buf;
}

std::string StripSuffix(std::string s, const char* suffix) {
    const size_t n = std::strlen(suffix);
    if (n > 0 && s.size() >= n && s.compare(s.size() - n, n, suffix) == 0) {
        s.erase(s.size() - n);
    }
    return s;
}

std::string StripCitySuffix(std::string s) {
    // "常州市" -> "常州"；UTF-8 的“市”占 3 字节。
    const char* shi = "市";
    const size_t n = std::strlen(shi);
    if (s.size() > n && s.compare(s.size() - n, n, shi) == 0) {
        s.erase(s.size() - n);
    }
    return s;
}

// reminder alert.color -> 中文颜色，用于详情页短预警文案。
std::string AlertColorName(const std::string& color) {
    if (color == "blue") return "蓝色";
    if (color == "yellow") return "黄色";
    if (color == "orange") return "橙色";
    if (color == "red") return "红色";
    if (color == "white") return "白色";
    return "";
}

}  // namespace

WeatherManager::WeatherManager() = default;

WeatherManager& WeatherManager::getInstance() {
    static WeatherManager instance;
    return instance;
}

void WeatherManager::loadConfigFromNvs() {
    Settings settings("weather", false);
    const std::string city = settings.GetString("city");
    const std::string adm = settings.GetString("adm");
    if (city.empty()) {
        ESP_LOGI(TAG, "天气城市：使用 reminder 服务端配置");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(loc_mutex_);
        city_ = city;
        adm_ = adm;
        manual_city_ = true;
        city_source_ = "手动配置";
    }
    ESP_LOGI(TAG, "天气城市：%s（手动配置）", city.c_str());
}

void WeatherManager::SetCity(const std::string& city, const std::string& adm) {
    if (city.empty()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(loc_mutex_);
        city_ = city;
        adm_ = adm;
        manual_city_ = true;
        city_source_ = "手动配置";
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        latest_data_ = WeatherData();
        latest_time_ = std::chrono::steady_clock::time_point{};
    }
    Settings wr("weather", true);
    wr.SetString("city", city);
    wr.SetString("adm", adm);
    ESP_LOGI(TAG, "天气城市已手动设置为 %s（上级=%s），等待下次拉取", city.c_str(),
             adm.empty() ? "-" : adm.c_str());
}

bool WeatherManager::updateFromReminderJson(const std::string& payload) {
    if (payload.empty()) {
        return false;
    }
    cJSON* root = cJSON_Parse(payload.c_str());
    if (root == nullptr) {
        ESP_LOGW(TAG, "reminder get_weather 响应不是 JSON");
        return false;
    }

    cJSON* result = cJSON_GetObjectItem(root, "result");
    cJSON* content = result ? cJSON_GetObjectItem(result, "content") : nullptr;
    cJSON* first = (content && cJSON_IsArray(content)) ? cJSON_GetArrayItem(content, 0) : nullptr;
    cJSON* text = first ? cJSON_GetObjectItem(first, "text") : nullptr;
    if (text == nullptr || !cJSON_IsString(text) || text->valuestring == nullptr) {
        ESP_LOGW(TAG, "reminder get_weather 响应缺少 result.content[0].text");
        cJSON_Delete(root);
        return false;
    }

    cJSON* weather = cJSON_Parse(text->valuestring);
    if (weather == nullptr) {
        ESP_LOGW(TAG, "reminder get_weather text 不是 JSON: %.96s", text->valuestring);
        cJSON_Delete(root);
        return false;
    }

    static WeatherData d;   // WeatherData 含多个 std::string，避免占满 MCP 任务栈
    d = WeatherData();

    cJSON* location = cJSON_GetObjectItem(weather, "location");
    cJSON* current = cJSON_GetObjectItem(weather, "current");
    const std::string city = StripCitySuffix(JsonString(location, "name"));
    if (city.empty() || current == nullptr) {
        ESP_LOGW(TAG, "reminder get_weather 缺少 location/current");
        cJSON_Delete(weather);
        cJSON_Delete(root);
        return false;
    }

    d.city = city;
    d.temp = JsonNumber(current, "temperature");
    d.text = JsonString(current, "condition");
    d.feels_like = JsonNumber(current, "feels_like");
    d.humidity = JsonNumber(current, "humidity");
    d.wind_dir = JsonString(current, "wind_direction");
    d.wind_scale = JsonNumber(current, "wind_scale");
    d.wind_speed = JsonNumber(current, "wind_speed", 1);
    d.vis = StripSuffix(JsonString(current, "visibility"), "km");
    d.pressure = JsonNumber(current, "pressure");
    d.precipitation = JsonNumber(current, "precipitation", 1);
    d.update_time = JsonString(current, "observed_at");
    if (d.update_time.empty()) {
        d.update_time = "reminder";
    }
    if (d.temp.empty() || d.text.empty()) {
        ESP_LOGW(TAG, "reminder get_weather 缺少 current.temperature/condition");
        cJSON_Delete(weather);
        cJSON_Delete(root);
        return false;
    }

    cJSON* days = cJSON_GetObjectItem(weather, "days");
    if (days && cJSON_IsArray(days)) {
        const int n = cJSON_GetArraySize(days);
        d.daily_count = (n > 3) ? 3 : n;
        for (int i = 0; i < d.daily_count; ++i) {
            cJSON* day = cJSON_GetArrayItem(days, i);
            WeatherDaily& wd = d.daily[i];
            wd.date = JsonString(day, "date");
            wd.week = JsonString(day, "weekday");
            wd.text_day = JsonString(day, "text_day");
            wd.text_night = JsonString(day, "text_night");
            wd.temp_min = JsonNumber(day, "temperature_min");
            wd.temp_max = JsonNumber(day, "temperature_max");
            wd.wind_dir = JsonString(day, "wind_direction");
            wd.wind_scale = JsonNumber(day, "wind_scale");
            wd.sunrise = JsonString(day, "sunrise");
            wd.sunset = JsonString(day, "sunset");
        }
        if (d.daily_count > 0) {
            d.sunrise = d.daily[0].sunrise;
            d.sunset = d.daily[0].sunset;
        }
    }

    cJSON* alerts = cJSON_GetObjectItem(weather, "alerts");
    if (alerts && cJSON_IsArray(alerts) && cJSON_GetArraySize(alerts) > 0) {
        cJSON* alert = cJSON_GetArrayItem(alerts, 0);
        d.warning = JsonString(alert, "headline");
        if (d.warning.empty()) {
            d.warning = JsonString(alert, "event_type");
        }
        // 详情页底栏只有一行，显示“寒潮蓝色预警”这类短文案，完整 headline 留给语音播报。
        std::string event_type = JsonString(alert, "event_type");
        std::string color_name = AlertColorName(JsonString(alert, "color"));
        if (!event_type.empty()) {
            d.warning_short = event_type + color_name + "预警";
        } else if (!d.warning.empty()) {
            d.warning_short = d.warning;
        }
    }

    d.valid = true;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        latest_data_ = d;
        latest_time_ = std::chrono::steady_clock::now();
    }
    {
        std::lock_guard<std::mutex> lock(loc_mutex_);
        if (!manual_city_) {
            city_ = d.city;
            city_source_ = "Reminder MCP";
        }
    }

    ESP_LOGI(TAG, "天气更新成功(reminder MCP): %s %s %s°C 体感%s 湿度%s%% %s %s级",
             d.city.c_str(), d.text.c_str(), d.temp.c_str(), d.feels_like.c_str(),
             d.humidity.c_str(), d.wind_dir.c_str(), d.wind_scale.c_str());

    cJSON_Delete(weather);
    cJSON_Delete(root);
    return true;
}

bool WeatherManager::updateFromExternal(const std::string& city,
                                        const std::string& weather_text,
                                        const std::string& temperature,
                                        const std::string& update_time) {
    static WeatherData d;
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
    {
        std::lock_guard<std::mutex> lock(mutex_);
        latest_data_ = d;
        latest_time_ = std::chrono::steady_clock::now();
    }
    ESP_LOGI(TAG, "天气已由外部写入: %s %s %s°C", d.city.c_str(), d.text.c_str(), d.temp.c_str());
    return true;
}
