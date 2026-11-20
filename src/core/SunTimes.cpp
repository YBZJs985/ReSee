#include "core/SunTimes.h"
#include "core/TimeUtils.h"

#include <QDate>
#include <algorithm>
#include <cmath>

namespace mcs {

namespace {
constexpr double kDegToRad = 3.14159265358979323846 / 180.0;
constexpr double kRadToDeg = 180.0 / 3.14159265358979323846;

double normDeg(double deg) {
    double r = std::fmod(deg, 360.0);
    if (r < 0.0) r += 360.0;
    return r;
}

double normHours(double h) {
    double r = std::fmod(h, 24.0);
    if (r < 0.0) r += 24.0;
    return r;
}

// USNO / NOAA 经典离线日出日落计算，返回本地当天小时数 [0, 24)
bool computeSunEventHour(int year, int month, int day,
                         double lat, double lon, double tzHours,
                         bool isSunrise, double& outLocalHour) {
    const QDate d(year, month, day);
    if (!d.isValid()) return false;
    const int n = d.dayOfYear();

    const double lngHour = lon / 15.0;
    const double t = isSunrise
        ? (static_cast<double>(n) + ((6.0 - lngHour) / 24.0))
        : (static_cast<double>(n) + ((18.0 - lngHour) / 24.0));

    const double m = (0.9856 * t) - 3.289;
    const double l = normDeg(m + (1.916 * std::sin(m * kDegToRad)) +
                             (0.020 * std::sin(2.0 * m * kDegToRad)) + 282.634);

    double ra = kRadToDeg * std::atan(0.91764 * std::tan(l * kDegToRad));
    ra = normDeg(ra);
    const double lQuad = std::floor(l / 90.0) * 90.0;
    const double raQuad = std::floor(ra / 90.0) * 90.0;
    ra = (ra + (lQuad - raQuad)) / 15.0;

    const double sinDec = 0.39782 * std::sin(l * kDegToRad);
    const double cosDec = std::cos(std::asin(sinDec));

    // 标准日出日落天顶角 90°50' (90.8333°)
    const double cosZenith = std::cos(90.8333 * kDegToRad);
    const double cosH = (cosZenith - (sinDec * std::sin(lat * kDegToRad))) /
                        (cosDec * std::cos(lat * kDegToRad));

    if (cosH > 1.0) {
        // 极夜：全天黑夜
        outLocalHour = isSunrise ? 12.0 : 12.0;
        return true;
    }
    if (cosH < -1.0) {
        // 极昼：全天白天
        outLocalHour = isSunrise ? 0.0 : 24.0;
        return true;
    }

    double h = isSunrise
        ? (360.0 - kRadToDeg * std::acos(cosH)) / 15.0
        : (kRadToDeg * std::acos(cosH)) / 15.0;

    const double localMeanTime = h + ra - (0.06571 * t) - 6.622;
    const double ut = normHours(localMeanTime - lngHour);
    outLocalHour = normHours(ut + tzHours);
    return true;
}
} // namespace

SunTimesService& SunTimesService::instance() {
    static SunTimesService s;
    return s;
}

void SunTimesService::configure(bool enabled, double latitude, double longitude, int tzOffsetSec) {
    QMutexLocker lk(&m_mtx);
    const double clampedLat = std::clamp(latitude, -89.9, 89.9);
    const double clampedLon = std::clamp(longitude, -180.0, 180.0);
    if (m_enabled == enabled &&
        std::abs(m_lat - clampedLat) < 1e-6 &&
        std::abs(m_lon - clampedLon) < 1e-6 &&
        m_tzOffsetSec == tzOffsetSec) {
        return;
    }
    m_enabled = enabled;
    m_lat = clampedLat;
    m_lon = clampedLon;
    m_tzOffsetSec = tzOffsetSec;
    m_cache.clear();
}

bool SunTimesService::isConfigured() const {
    QMutexLocker lk(&m_mtx);
    return m_enabled;
}

DaySunTimes SunTimesService::getForDay(int64_t anyWallMs) const {
    int tz = 8 * 3600;
    {
        QMutexLocker lk(&m_mtx);
        tz = m_tzOffsetSec;
    }
    return getForDayStart(TimeUtils::dayStartMs(anyWallMs, tz));
}

DaySunTimes SunTimesService::getForDayStart(int64_t dayStartMs) const {
    {
        QMutexLocker lk(&m_mtx);
        auto it = m_cache.constFind(dayStartMs);
        if (it != m_cache.constEnd()) {
            return it.value();
        }
    }
    const DaySunTimes computed = computeForDayStart(dayStartMs);
    {
        QMutexLocker lk(&m_mtx);
        m_cache.insert(dayStartMs, computed);
    }
    return computed;
}

DaySunTimes SunTimesService::computeForDayStart(int64_t dayStartMs) const {
    bool enabled = false;
    double lat = 39.9042;
    double lon = 116.4074;
    int tz = 8 * 3600;
    {
        QMutexLocker lk(&m_mtx);
        enabled = m_enabled;
        lat = m_lat;
        lon = m_lon;
        tz = m_tzOffsetSec;
    }

    DaySunTimes res;
    res.dayStartMs = dayStartMs;
    res.isCalculated = enabled;

    if (!enabled) {
        // 未配置地理位置时使用明确的固定 06:00 - 18:00 默认划分
        res.sunriseMs = dayStartMs + 6LL * 3600 * 1000LL;
        res.sunsetMs = dayStartMs + 18LL * 3600 * 1000LL;
        res.sunriseFrac = 0.25;
        res.sunsetFrac = 0.75;
        return res;
    }

    int y = 0, m = 0, d = 0, h = 0, mi = 0, s = 0, ms = 0;
    TimeUtils::epochMsToCivil(dayStartMs, y, m, d, h, mi, s, ms, tz);

    double riseH = 6.0;
    double setH = 18.0;
    const double tzHours = static_cast<double>(tz) / 3600.0;
    computeSunEventHour(y, m, d, lat, lon, tzHours, true, riseH);
    computeSunEventHour(y, m, d, lat, lon, tzHours, false, setH);

    if (setH < riseH) {
        std::swap(riseH, setH);
    }
    res.sunriseMs = dayStartMs + static_cast<int64_t>(std::llround(riseH * 3600000.0));
    res.sunsetMs = dayStartMs + static_cast<int64_t>(std::llround(setH * 3600000.0));
    res.sunriseFrac = std::clamp(riseH / 24.0, 0.0, 1.0);
    res.sunsetFrac = std::clamp(setH / 24.0, 0.0, 1.0);
    return res;
}

} // namespace mcs
