#pragma once
#include <string>
#include <mutex>
#include <chrono>

// 单日预报（reminder MCP get_weather 返回的 days 数组）
struct WeatherDaily {
    std::string date;       // 2026-11-24
    std::string week;       // 周二
    std::string text_day;   // 白天天气：晴
    std::string text_night; // 夜间天气：阴
    std::string temp_min;   // 最低温
    std::string temp_max;   // 最高温
    std::string wind_dir;   // 西北偏北风
    std::string wind_scale; // 1-3
    std::string sunrise;    // 06:36
    std::string sunset;     // 16:57
};

// 屏幕/语音使用的天气快照。本板天气统一来自 reminder MCP 的 get_weather，
// 不再由固件直连 QWeather / 中国天气网 / Open-Meteo。
struct WeatherData {
    std::string city;     // 城市名
    std::string temp;     // 温度（字符串）
    std::string text;     // 天气描述（如"晴"、"多云"）
    std::string update_time;
    bool valid = false;

    std::string feels_like;   // 体感温度
    std::string humidity;     // 相对湿度 %
    std::string wind_dir;     // 风向：西北风
    std::string wind_scale;   // 风力等级：3
    std::string wind_speed;   // 风速 m/s（reminder 提供）
    std::string vis;          // 能见度 km（仅数字）
    std::string pressure;     // 气压 hPa（仅数字）
    std::string precipitation;// 降水量 mm
    std::string aqi;          // 空气质量指数（reminder 当前不提供时为空）
    std::string aqi_category; // 优 / 良 / 轻度污染 ...
    std::string pm25;         // PM2.5
    std::string sunrise;      // 今日日出 HH:MM
    std::string sunset;       // 今日日落 HH:MM
    std::string warning;      // 当前生效预警 headline；无预警为空，语音播报用
    std::string warning_short;// 详情页短文案：如“寒潮蓝色预警”
    WeatherDaily daily[3];    // 未来 3 天（含今天）
    int daily_count = 0;
};

// 天气缓存。天气数据由 TodoMcpClient 通过同一 reminder MCP 长连接调用
// get_weather 获取，这里只负责保存线程安全的快照和手动城市。
class WeatherManager {
public:
    static WeatherManager& getInstance();

    // 获取最新天气数据（线程安全，直接读取缓存）
    WeatherData getLatestData() {
        std::lock_guard<std::mutex> lock(mutex_);
        // 天气每 12 小时才刷新一次，缓存保留 13 小时，避免两次拉取之间提前失效。
        if (latest_data_.valid && latest_time_ != std::chrono::steady_clock::time_point{}) {
            if (std::chrono::steady_clock::now() - latest_time_ > std::chrono::hours(13)) {
                latest_data_.valid = false;
            }
        }
        return latest_data_;
    }

    // 从 NVS weather/city、weather/adm 读取用户手动城市。
    void loadConfigFromNvs();

    // 当前查询城市（空字符串表示使用 reminder 服务端配置的城市）。
    std::string city() const { std::lock_guard<std::mutex> lock(loc_mutex_); return city_; }
    std::string citySource() const { std::lock_guard<std::mutex> lock(loc_mutex_); return city_source_; }

    // 手动指定城市（MCP self.weather.set_city 用），持久化到 NVS。
    void SetCity(const std::string& city, const std::string& adm = "");

    // 解析 reminder MCP 的 get_weather 响应（result.content[0].text 为 JSON 字符串）。
    bool updateFromReminderJson(const std::string& payload);

    // 兼容 AI 侧 MCP 直接写入天气缓存（不参与自动拉取）。
    bool updateFromExternal(const std::string& city,
                            const std::string& weather_text,
                            const std::string& temperature,
                            const std::string& update_time = "");

private:
    WeatherManager();
    mutable std::mutex mutex_;       // 保护 latest_data_ / latest_time_
    mutable std::mutex loc_mutex_;   // 保护 city_ / adm_ / city_source_ / manual_city_
    WeatherData latest_data_;
    std::chrono::steady_clock::time_point latest_time_{};
    std::string city_;
    std::string adm_;
    std::string city_source_ = "Reminder MCP";
    bool manual_city_ = false;
};
