#include "core/TimeUtils.h"

#include <QRegularExpression>
#include <chrono>

namespace mcs::TimeUtils {

namespace {
// Howard Hinnant civil-from-days / days-from-civil 算法（线程安全，O(1)，无时区状态依赖）
constexpr int64_t daysFromCivil(int y, unsigned m, unsigned d) noexcept {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

constexpr void civilFromDays(int64_t z, int& y, int& m, int& d) noexcept {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = static_cast<int>(yoe) + static_cast<int>(era * 400);
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    m = static_cast<int>(mp + (mp < 10 ? 3 : -9));
    y += (m <= 2);
}

bool parse14Digits(const QString& s, int& y, int& mo, int& d, int& h, int& mi, int& se) {
    if (s.size() != 14) return false;
    bool ok1 = false, ok2 = false, ok3 = false, ok4 = false, ok5 = false, ok6 = false;
    y  = QStringView(s).mid(0, 4).toInt(&ok1);
    mo = QStringView(s).mid(4, 2).toInt(&ok2);
    d  = QStringView(s).mid(6, 2).toInt(&ok3);
    h  = QStringView(s).mid(8, 2).toInt(&ok4);
    mi = QStringView(s).mid(10, 2).toInt(&ok5);
    se = QStringView(s).mid(12, 2).toInt(&ok6);
    if (!ok1 || !ok2 || !ok3 || !ok4 || !ok5 || !ok6) return false;
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 || mi < 0 || mi > 59 || se < 0 || se > 59) {
        return false;
    }
    return true;
}
} // namespace

int64_t civilToEpochMs(
    int year, int month, int day,
    int hour, int minute, int second,
    int ms, int tzOffsetSec) {
    const int64_t days = daysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
    const int64_t secUtc = days * 86400LL + hour * 3600LL + minute * 60LL + second - tzOffsetSec;
    return secUtc * 1000LL + ms;
}

void epochMsToCivil(
    int64_t epochMs,
    int& year, int& month, int& day,
    int& hour, int& minute, int& second, int& ms,
    int tzOffsetSec) {
    int64_t localMs = epochMs + static_cast<int64_t>(tzOffsetSec) * 1000LL;
    int64_t remMs = localMs % 1000LL;
    if (remMs < 0) remMs += 1000LL;
    int64_t totalSec = (localMs - remMs) / 1000LL;
    int64_t secOfDay = totalSec % 86400LL;
    if (secOfDay < 0) secOfDay += 86400LL;
    int64_t days = (totalSec - secOfDay) / 86400LL;

    civilFromDays(days, year, month, day);
    hour = static_cast<int>(secOfDay / 3600LL);
    minute = static_cast<int>((secOfDay % 3600LL) / 60LL);
    second = static_cast<int>(secOfDay % 60LL);
    ms = static_cast<int>(remMs);
}

std::optional<ParsedFilename> parseXiaomiFilename(const QString& fileName, int tzOffsetSec) {
    static const QRegularExpression re(QStringLiteral(R"(^(\d{2})_(\d{14})_(\d{14})\.mp4$)"),
                                       QRegularExpression::CaseInsensitiveOption);
    const auto m = re.match(fileName);
    if (!m.hasMatch()) {
        return std::nullopt;
    }

    int y1, mo1, d1, h1, mi1, s1;
    int y2, mo2, d2, h2, mi2, s2;
    if (!parse14Digits(m.captured(2), y1, mo1, d1, h1, mi1, s1)) return std::nullopt;
    if (!parse14Digits(m.captured(3), y2, mo2, d2, h2, mi2, s2)) return std::nullopt;

    ParsedFilename res;
    res.channel = m.captured(1);
    res.nominalStartMs = civilToEpochMs(y1, mo1, d1, h1, mi1, s1, 0, tzOffsetSec);
    res.nominalEndMs = civilToEpochMs(y2, mo2, d2, h2, mi2, s2, 0, tzOffsetSec);
    if (res.nominalEndMs < res.nominalStartMs) {
        return std::nullopt;
    }
    return res;
}

std::optional<int64_t> parseDateTimeString(const QString& text, int tzOffsetSec) {
    const QString trimmed = text.trimmed();
    if (trimmed.size() == 14) {
        int y, mo, d, h, mi, s;
        if (parse14Digits(trimmed, y, mo, d, h, mi, s)) {
            return civilToEpochMs(y, mo, d, h, mi, s, 0, tzOffsetSec);
        }
    }
    static const QRegularExpression re(
        QStringLiteral(R"(^(\d{4})[-/](\d{1,2})[-/](\d{1,2})[ T_](\d{1,2}):(\d{1,2}):(\d{1,2})(?:\.(\d{1,3}))?$)"));
    const auto m = re.match(trimmed);
    if (!m.hasMatch()) return std::nullopt;

    int y = m.captured(1).toInt();
    int mo = m.captured(2).toInt();
    int d = m.captured(3).toInt();
    int h = m.captured(4).toInt();
    int mi = m.captured(5).toInt();
    int s = m.captured(6).toInt();
    int ms = 0;
    if (!m.captured(7).isEmpty()) {
        QString msStr = m.captured(7).leftJustified(3, '0').left(3);
        ms = msStr.toInt();
    }
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 || mi < 0 || mi > 59 || s < 0 || s > 59) {
        return std::nullopt;
    }
    return civilToEpochMs(y, mo, d, h, mi, s, ms, tzOffsetSec);
}

QString formatEpochMs(int64_t epochMs, int tzOffsetSec, bool withMillis) {
    int y, mo, d, h, mi, s, ms;
    epochMsToCivil(epochMs, y, mo, d, h, mi, s, ms, tzOffsetSec);
    if (withMillis) {
        return QStringLiteral("%1-%2-%3 %4:%5:%6.%7")
            .arg(y, 4, 10, QLatin1Char('0'))
            .arg(mo, 2, 10, QLatin1Char('0'))
            .arg(d, 2, 10, QLatin1Char('0'))
            .arg(h, 2, 10, QLatin1Char('0'))
            .arg(mi, 2, 10, QLatin1Char('0'))
            .arg(s, 2, 10, QLatin1Char('0'))
            .arg(ms, 3, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1-%2-%3 %4:%5:%6")
        .arg(y, 4, 10, QLatin1Char('0'))
        .arg(mo, 2, 10, QLatin1Char('0'))
        .arg(d, 2, 10, QLatin1Char('0'))
        .arg(h, 2, 10, QLatin1Char('0'))
        .arg(mi, 2, 10, QLatin1Char('0'))
        .arg(s, 2, 10, QLatin1Char('0'));
}

QString formatTimeOnlyMs(int64_t epochMs, int tzOffsetSec) {
    int y, mo, d, h, mi, s, ms;
    epochMsToCivil(epochMs, y, mo, d, h, mi, s, ms, tzOffsetSec);
    return QStringLiteral("%1:%2:%3")
        .arg(h, 2, 10, QLatin1Char('0'))
        .arg(mi, 2, 10, QLatin1Char('0'))
        .arg(s, 2, 10, QLatin1Char('0'));
}

QString formatDateOnlyMs(int64_t epochMs, int tzOffsetSec) {
    int y, mo, d, h, mi, s, ms;
    epochMsToCivil(epochMs, y, mo, d, h, mi, s, ms, tzOffsetSec);
    return QStringLiteral("%1-%2-%3")
        .arg(y, 4, 10, QLatin1Char('0'))
        .arg(mo, 2, 10, QLatin1Char('0'))
        .arg(d, 2, 10, QLatin1Char('0'));
}

int64_t dayStartMs(int64_t epochMs, int tzOffsetSec) {
    int y, mo, d, h, mi, s, ms;
    epochMsToCivil(epochMs, y, mo, d, h, mi, s, ms, tzOffsetSec);
    return civilToEpochMs(y, mo, d, 0, 0, 0, 0, tzOffsetSec);
}

int64_t nowEpochMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace mcs::TimeUtils
