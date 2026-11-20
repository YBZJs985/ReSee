#pragma once

#include <QHash>
#include <QMutex>
#include <cstdint>

namespace mcs {

struct DaySunTimes {
    int64_t dayStartMs = 0;
    int64_t sunriseMs = 0;
    int64_t sunsetMs = 0;
    double sunriseFrac = 0.25; // 默认 06:00
    double sunsetFrac = 0.75;  // 默认 18:00
    bool isCalculated = false; // true=天文计算，false=固定 06:00-18:00
};

class SunTimesService {
public:
    static SunTimesService& instance();

    void configure(bool enabled, double latitude, double longitude, int tzOffsetSec);
    bool isConfigured() const;

    // 按日期缓存返回当天的日出日落时间（线程安全，O(1) 缓存命中）
    DaySunTimes getForDay(int64_t anyWallMs) const;
    DaySunTimes getForDayStart(int64_t dayStartMs) const;

private:
    SunTimesService() = default;
    DaySunTimes computeForDayStart(int64_t dayStartMs) const;

    mutable QMutex m_mtx;
    bool m_enabled = false;
    double m_lat = 39.9042;
    double m_lon = 116.4074;
    int m_tzOffsetSec = 8 * 3600;
    mutable QHash<int64_t, DaySunTimes> m_cache;
};

} // namespace mcs
