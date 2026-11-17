#ifndef __DATE_UTILS_H__
#define __DATE_UTILS_H__

// 星期中文名共用实现（CODE_REVIEW P3 #6）
// weather_manager.cc 的 DateToWeek() 与 data_update_task.cc 的 weeks_zh[]
// 原先各自维护一份「日一二三四五六」，改文案时容易只改一处。
namespace date_utils {

// 0=周日 … 6=周六；越界按周日兜底（与原先数组下标行为一致）
inline const char* WeekdayCharZh(int tm_wday) {
    static const char* kNames[] = {"日", "一", "二", "三", "四", "五", "六"};
    return (tm_wday >= 0 && tm_wday < 7) ? kNames[tm_wday] : kNames[0];
}

}  // namespace date_utils

#endif  // __DATE_UTILS_H__
