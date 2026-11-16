
import math, datetime, sys

def jd_from_dt(y, m, d, h=0.0):
    if m <= 2:
        y -= 1; m += 12
    A = y // 100
    B = 2 - A + A // 4
    return math.floor(365.25*(y+4716)) + math.floor(30.6001*(m+1)) + d + B - 1524.5 + h/24.0

def dt_from_jd(jd):
    jd += 0.5
    Z = int(math.floor(jd)); F = jd - Z
    if Z < 2299161:
        A = Z
    else:
        alpha = int((Z - 1867216.25)/36524.25)
        A = Z + 1 + alpha - alpha//4
    B = A + 1524
    C = int((B - 122.1)/365.25)
    D = int(365.25*C)
    E = int((B - D)/30.6001)
    day = B - D - int(30.6001*E) + F
    month = E - 1 if E < 14 else E - 13
    year = C - 4716 if month > 2 else C - 4715
    return year, month, int(math.floor(day)), (day % 1)*24.0

def delta_t(y):
    if 1900 <= y < 1920:
        t = y-1900; return -2.79 + 1.494119*t - 0.0598939*t**2 + 0.0061966*t**3 - 0.000197*t**4
    if 1920 <= y < 1941:
        t = y-1920; return 21.20 + 0.84493*t - 0.076100*t**2 + 0.0020936*t**3
    if 1941 <= y < 1961:
        t = y-1950; return 29.07 + 0.407*t - t**2/233 + t**3/2547
    if 1961 <= y < 1986:
        t = y-1975; return 45.45 + 1.067*t - t**2/260 - t**3/718
    if 1986 <= y < 2005:
        t = y-2000; return 63.86 + 0.3345*t - 0.060374*t**2 + 0.0017275*t**3 + 0.000651814*t**4 + 0.00002373599*t**5
    if 2005 <= y < 2050:
        t = y-2000; return 62.92 + 0.32217*t + 0.005589*t**2
    u = (y-1820)/100.0; return -20 + 32*u*u - 0.5628*(2150-y)

def sun_apparent_long(jd):
    T = (jd - 2451545.0)/36525.0
    L0 = 280.46646 + 36000.76983*T + 0.0003032*T*T
    M = math.radians(357.52911 + 35999.05029*T - 0.0001537*T*T)
    C = ((1.914602 - 0.004817*T - 0.000014*T*T)*math.sin(M)
         + (0.019993 - 0.000101*T)*math.sin(2*M) + 0.000289*math.sin(3*M))
    omega = math.radians(125.04 - 1934.136*T)
    return (L0 + C - 0.00569 - 0.00478*math.sin(omega)) % 360.0

def new_moon_jde(k):
    T = k/1236.85
    JDE = 2451550.09766 + 29.530588861*k + 0.00015437*T**2 - 0.000000150*T**3 + 0.00000000073*T**4
    E = 1 - 0.002516*T - 0.0000074*T**2
    M = math.radians(2.5534 + 29.10535670*k - 0.0000014*T**2 - 0.00000011*T**3)
    Mp = math.radians(201.5643 + 385.81693528*k + 0.0107582*T**2 + 0.00001238*T**3 - 0.000000058*T**4)
    F = math.radians(160.7108 + 390.67050284*k - 0.0016118*T**2 - 0.00000227*T**3 + 0.000000011*T**4)
    Om = math.radians(124.7746 - 1.56375588*k + 0.0020672*T**2 + 0.00000215*T**3)
    s = math.sin
    corr = (-0.40720*s(Mp) + 0.17241*E*s(M) + 0.01608*s(2*Mp) + 0.01039*s(2*F)
            + 0.00739*E*s(Mp-M) - 0.00514*E*s(Mp+M) + 0.00208*E*E*s(2*M)
            - 0.00111*s(Mp-2*F) - 0.00057*s(Mp+2*F) + 0.00056*E*s(2*Mp+M)
            - 0.00042*s(3*Mp) + 0.00042*E*s(M+2*F) + 0.00038*E*s(M-2*F)
            - 0.00024*E*s(2*Mp-M) - 0.00017*s(Om) - 0.00007*s(Mp+2*M)
            + 0.00004*s(2*Mp-2*F) + 0.00004*s(3*M) + 0.00003*s(Mp+M-2*F)
            + 0.00003*s(2*Mp+2*F) - 0.00003*s(Mp+M+2*F) + 0.00003*s(Mp-M+2*F)
            - 0.00002*s(Mp-M-2*F) - 0.00002*s(3*Mp+M) + 0.00002*s(4*Mp))
    A = [(299.77+0.107408*k-0.009173*T**2,0.000325),(251.88+0.016321*k,0.000165),(251.83+26.651886*k,0.000164),
         (349.42+36.412478*k,0.000126),(84.66+18.206239*k,0.000110),(141.74+53.303771*k,0.000062),
         (207.14+2.453732*k,0.000060),(154.84+7.306860*k,0.000056),(34.52+27.261239*k,0.000047),
         (207.19+0.121824*k,0.000042),(291.34+1.844379*k,0.000040),(161.72+24.198154*k,0.000037),
         (239.56+25.513099*k,0.000035),(331.55+3.592518*k,0.000023)]
    for ang, amp in A:
        corr += amp*math.sin(math.radians(ang))
    return JDE + corr

def new_moon_utc(k):
    jde = new_moon_jde(k)
    y = dt_from_jd(jde)[0]
    return jde - delta_t(y)/86400.0

def beijing_day(jd_utc):
    y, m, d, h = dt_from_jd(jd_utc + 8.0/24.0)
    return datetime.date(y, m, d)

TERM_MD = {315:(2,4),330:(2,19),345:(3,5),0:(3,20),15:(4,5),30:(4,20),45:(5,6),60:(5,21),
           75:(6,6),90:(6,21),105:(7,7),120:(7,23),135:(8,8),150:(8,23),165:(9,8),180:(9,23),
           195:(10,8),210:(10,23),225:(11,7),240:(11,22),255:(12,7),270:(12,22),285:(1,6),300:(1,20)}
TERM_ORDER = [285,300,315,330,345,0,15,30,45,60,75,90,105,120,135,150,165,180,195,210,225,240,255,270]
TERM_CN = ["小寒","大寒","立春","雨水","惊蛰","春分","清明","谷雨","立夏","小满","芒种","夏至",
           "小暑","大暑","立秋","处暑","白露","秋分","寒露","霜降","立冬","小雪","大雪","冬至"]

def term_day(year, angle):
    m, d = TERM_MD[angle]
    lo = jd_from_dt(year, m, d) - 6.0
    hi = jd_from_dt(year, m, d) + 6.0
    for _ in range(60):
        mid = (lo+hi)/2
        f = (sun_apparent_long(mid) - angle + 180) % 360 - 180
        if f < 0: lo = mid
        else: hi = mid
    return beijing_day((lo+hi)/2)

Y_MIN, Y_MAX = 1900, 2100
months = sorted({beijing_day(new_moon_utc(k)) for k in range(int((1899-2000)*12.3685)-4, int((2102-2000)*12.3685)+4)})
months = [d for d in months if datetime.date(1899,1,1) <= d <= datetime.date(2102,12,31)]

def month_idx(day):
    lo, hi = 0, len(months)-1
    while lo <= hi:
        mid = (lo+hi)//2
        if months[mid] <= day: lo = mid+1
        else: hi = mid-1
    return hi  # index whose start <= day

def month_len(i):
    return (months[i+1]-months[i]).days

MAJOR = list(range(0,360,30))
_major_cache = {}
def major_days(y):
    if y not in _major_cache:
        _major_cache[y] = [term_day(y,a) for a in MAJOR]
    return _major_cache[y]

def has_major(i):
    s, e = months[i], months[i+1]
    for y in (s.year, e.year):
        for d in major_days(y):
            if s <= d < e: return True
    return False

ASSIGN = {}
for Y in range(Y_MIN-1, Y_MAX+2):
    a = month_idx(term_day(Y-1, 270))
    b = month_idx(term_day(Y, 270))
    cnt = b - a
    base = [11,12] + list(range(1,11))
    if cnt == 12:
        for off in range(12):
            n = base[off]; ASSIGN[a+off] = (Y-1 if n >= 11 else Y, n, False)
    elif cnt == 13:
        leap_off = next(off for off in range(1,13) if not has_major(a+off))
        for off in range(13):
            if off < leap_off: n = base[off]; ASSIGN[a+off] = (Y-1 if n>=11 else Y, n, False)
            elif off == leap_off: n = base[off-1]; ASSIGN[a+off] = (Y-1 if n>=11 else Y, n, True)
            else: n = base[off-1]; ASSIGN[a+off] = (Y-1 if n>=11 else Y, n, False)
    else:
        raise SystemExit("bad month count %d for %d" % (cnt, Y))

def year_table(Y):
    ents = [(i, v[1], v[2]) for i, v in sorted(ASSIGN.items()) if v[0] == Y]
    if not ents or ents[0][1] != 1 or ents[0][2]:
        raise SystemExit("bad start for %d: %s" % (Y, ents[:2]))
    leap_m, leap_len, lengths = 0, 0, {}
    for i, n, lp in ents:
        if lp: leap_m, leap_len = n, month_len(i)
        else: lengths[n] = month_len(i)
    if sorted(lengths) != list(range(1,13)):
        raise SystemExit("bad months %d: %s" % (Y, sorted(lengths)))
    return leap_m, leap_len, [lengths[i] for i in range(1,13)], months[ents[0][0]]

rows = {}
for Y in range(Y_MIN, Y_MAX+1):
    leap_m, leap_len, lengths, zheng = year_table(Y)
    info = leap_m & 0xF
    for i, L in enumerate(lengths):
        if L == 30: info |= 1 << (4+i)
    if leap_m and leap_len == 30: info |= 1 << 16
    days_total = 29*12 + sum(1 for L in lengths if L == 30) + (29 + (1 if leap_len == 30 else 0) if leap_m else 0)
    rows[Y] = (info, leap_m, leap_len, lengths, zheng, days_total)

# validations
bad = []
for Y in range(Y_MIN, Y_MAX):
    real = (rows[Y+1][4] - rows[Y][4]).days
    if real != rows[Y][5]: bad.append((Y, rows[Y][5], real))
    if not (datetime.date(Y,1,21) <= rows[Y][4] <= datetime.date(Y,2,21)): bad.append(("zc", Y, rows[Y][4]))
    if any(L not in (29,30) for L in rows[Y][3]): bad.append(("len", Y))
print("table errors:", bad[:5], "count", len(bad))

anchors = {2020:(1,25),2021:(2,12),2022:(2,1),2023:(1,22),2024:(2,10),2025:(1,29),2026:(2,17),2027:(2,6),2028:(1,26),2029:(2,13),2030:(2,3)}
ok = all((rows[y][4].month, rows[y][4].day) == v for y, v in anchors.items())
print("spring festival anchors:", ok)

# term table
terms = {}
for Y in range(Y_MIN, Y_MAX+1):
    terms[Y] = [term_day(Y, a).day for a in TERM_ORDER]
print("term sample 2025:", list(zip(TERM_CN, terms[2025]))[:4], "...")

# emit C++
def cpp_array_u32(name, vals):
    lines = ["static const uint32_t %s[%d] = {" % (name, len(vals))]
    for i in range(0, len(vals), 12):
        lines.append("    " + ", ".join("0x%06X" % v for v in vals[i:i+12]) + ",")
    lines.append("};")
    return "\n".join(lines)

def cpp_array_u8(name, vals, per=24):
    lines = ["static const uint8_t %s[%d] = {" % (name, len(vals))]
    for i in range(0, len(vals), per):
        lines.append("    " + ", ".join("%2d" % v for v in vals[i:i+per]) + ",")
    lines.append("};")
    return "\n".join(lines)

info_vals = [rows[Y][0] for Y in range(Y_MIN, Y_MAX+1)]
term_vals = []
for Y in range(Y_MIN, Y_MAX+1):
    term_vals.extend(terms[Y])

header = """// Auto-generated by tools/gen_lunar_table.py — do not edit by hand.
// Lunar calendar (1900-2100) with solar terms, table-driven integer arithmetic.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace lunar {

struct Date {
    int year;   // lunar year
    int month;  // 1..12
    int day;    // 1..30
    bool is_leap;
};

constexpr int kMinYear = 1900;
constexpr int kMaxYear = 2100;

// Convert a Gregorian date to the Chinese lunar date.
// Supported range: 1900-01-31 .. 2100-12-31 (inclusive). Returns false outside.
bool SolarToLunar(int sy, int sm, int sd, Date& out);

// Days in a lunar month (29 or 30); 0 when the date is out of range.
int MonthDays(const Date& d);

const char* MonthName(int month);                 // 1..12 -> "正月".."十二月"
const char* DayName(int day);                     // 1..30 -> "初一".."三十"
const char* SolarTermName(int index);             // 0..23 -> "小寒".."冬至"

// Solar term index (0..23, 小寒..冬至) falling on the given Gregorian day, or -1.
int SolarTermOfDay(int year, int month, int day);

const char* LunarFestival(const Date& d);         // nullptr when none
const char* SolarFestival(int month, int day);    // nullptr when none

// Build a compact UI label such as "十月初七 小雪" or "八月十五 中秋".
// Falls back to the lunar date alone when no festival/term applies.
bool FormatLabel(int sy, int sm, int sd, char* buf, size_t len);

}  // namespace lunar
"""

source_head = """// Auto-generated tables — do not edit by hand (see tools/gen_lunar_table.py).
#include "lunar_calendar.h"

#include <stdio.h>
#include <string.h>

namespace lunar {
namespace {

// Days since 1970-01-01 (Howard Hinnant's civil_from_days inverse).
constexpr int64_t DaysFromCivil(int y, int m, int d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

// 1900-01-31 is lunar 1900-01-01.
constexpr int64_t kEpochDays = DaysFromCivil(1900, 1, 31);

"""

source_mid = """
int YearIndex(int y) { return y - kMinYear; }

int YearDays(int info) {
    int days = 29 * 12;
    uint32_t bits = (info >> 4) & 0xFFF;
    while (bits) { days += bits & 1; bits >>= 1; }
    const int leap = info & 0xF;
    if (leap) days += 29 + ((info >> 16) & 1);
    return days;
}

int MonthLen(int info, int month, bool leap) {
    if (leap) return 29 + ((info >> 16) & 1);
    return 29 + ((info >> (4 + month - 1)) & 1);
}

}  // namespace

bool SolarToLunar(int sy, int sm, int sd, Date& out) {
    if (sy < kMinYear || sy > kMaxYear) return false;
    if (sm < 1 || sm > 12 || sd < 1 || sd > 31) return false;
    int64_t offset = DaysFromCivil(sy, sm, sd) - kEpochDays;
    if (offset < 0) return false;
    int year = kMinYear;
    for (; year <= kMaxYear; ++year) {
        const int days = YearDays(kLunarYearInfo[YearIndex(year)]);
        if (offset < days) break;
        offset -= days;
    }
    if (year > kMaxYear) return false;
    const int info = kLunarYearInfo[YearIndex(year)];
    const int leap = info & 0xF;
    for (int m = 1; m <= 12; ++m) {
        const int len = MonthLen(info, m, false);
        if (offset < len) { out = Date{year, m, static_cast<int>(offset) + 1, false}; return true; }
        offset -= len;
        if (leap == m) {
            const int llen = MonthLen(info, m, true);
            if (offset < llen) { out = Date{year, m, static_cast<int>(offset) + 1, true}; return true; }
            offset -= llen;
        }
    }
    return false;
}

int MonthDays(const Date& d) {
    if (d.year < kMinYear || d.year > kMaxYear || d.month < 1 || d.month > 12) return 0;
    const int info = kLunarYearInfo[YearIndex(d.year)];
    if (d.is_leap && (info & 0xF) != d.month) return 0;  // year has no such leap month
    return MonthLen(info, d.month, d.is_leap);
}

const char* MonthName(int month) {
    static const char* kNames[12] = {"正月", "二月", "三月", "四月", "五月", "六月",
                                     "七月", "八月", "九月", "十月", "十一月", "十二月"};
    return (month >= 1 && month <= 12) ? kNames[month - 1] : "";
}

const char* DayName(int day) {
    static const char* kNames[30] = {"初一", "初二", "初三", "初四", "初五", "初六", "初七", "初八", "初九", "初十",
                                     "十一", "十二", "十三", "十四", "十五", "十六", "十七", "十八", "十九", "二十",
                                     "廿一", "廿二", "廿三", "廿四", "廿五", "廿六", "廿七", "廿八", "廿九", "三十"};
    return (day >= 1 && day <= 30) ? kNames[day - 1] : "";
}

const char* SolarTermName(int index) {
    static const char* kNames[24] = {"小寒", "大寒", "立春", "雨水", "惊蛰", "春分", "清明", "谷雨",
                                     "立夏", "小满", "芒种", "夏至", "小暑", "大暑", "立秋", "处暑",
                                     "白露", "秋分", "寒露", "霜降", "立冬", "小雪", "大雪", "冬至"};
    return (index >= 0 && index < 24) ? kNames[index] : "";
}

int SolarTermOfDay(int year, int month, int day) {
    if (year < kMinYear || year > kMaxYear || month < 1 || month > 12) return -1;
    const uint8_t* row = &kSolarTermDay[YearIndex(year) * 24 + (month - 1) * 2];
    if (row[0] == day) return (month - 1) * 2;
    if (row[1] == day) return (month - 1) * 2 + 1;
    return -1;
}

const char* LunarFestival(const Date& d) {
    if (d.is_leap) return nullptr;
    if (d.month == 1 && d.day == 1) return "春节";
    if (d.month == 1 && d.day == 15) return "元宵";
    if (d.month == 5 && d.day == 5) return "端午";
    if (d.month == 7 && d.day == 7) return "七夕";
    if (d.month == 8 && d.day == 15) return "中秋";
    if (d.month == 9 && d.day == 9) return "重阳";
    if (d.month == 12 && d.day == 8) return "腊八";
    if (d.month == 12 && d.day == MonthDays(d)) return "除夕";
    return nullptr;
}

const char* SolarFestival(int month, int day) {
    if (month == 1 && day == 1) return "元旦";
    if (month == 3 && day == 8) return "妇女节";
    if (month == 5 && day == 1) return "劳动节";
    if (month == 6 && day == 1) return "儿童节";
    if (month == 9 && day == 10) return "教师节";
    if (month == 10 && day == 1) return "国庆节";
    if (month == 12 && day == 25) return "圣诞节";
    return nullptr;
}

bool FormatLabel(int sy, int sm, int sd, char* buf, size_t len) {
    if (!buf || len == 0) return false;
    Date d;
    if (!SolarToLunar(sy, sm, sd, d)) { buf[0] = '\\0'; return false; }
    const char* extra = LunarFestival(d);
    if (!extra) extra = SolarFestival(sm, sd);
    if (!extra) {
        const int term = SolarTermOfDay(sy, sm, sd);
        if (term >= 0) extra = SolarTermName(term);
    }
    if (extra && extra[0]) {
        snprintf(buf, len, "%s%s %s", MonthName(d.month), DayName(d.day), extra);
    } else {
        snprintf(buf, len, "%s%s", MonthName(d.month), DayName(d.day));
    }
    return true;
}

}  // namespace lunar
"""

out_h = header
out_cc = source_head + cpp_array_u32("kLunarYearInfo", info_vals) + "\n\n" + cpp_array_u8("kSolarTermDay", term_vals) + "\n" + source_mid

import os
base = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "managers") + os.sep
open(base + "lunar_calendar.h", "w", encoding="utf-8").write(out_h)
open(base + "lunar_calendar.cc", "w", encoding="utf-8").write(out_cc)
print("written: %d bytes header, %d bytes source" % (len(out_h), len(out_cc)))
print("info range check 2025: 0x%06X" % rows[2025][0], "leap", rows[2025][1])