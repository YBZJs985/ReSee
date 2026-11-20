#include "app/TimelineModel.h"
#include "core/SunTimes.h"
#include "core/TimeUtils.h"
#include "sync/SyncScheduler.h"

#include <QVariantMap>
#include <algorithm>
#include <cmath>

namespace mcs {

TimelineModel::TimelineModel(SyncScheduler* scheduler, int tzOffsetSec, QObject* parent)
    : QObject(parent), m_scheduler(scheduler), m_tzOffsetSec(tzOffsetSec) {}

qint64 TimelineModel::totalMinMs() const {
    return m_scheduler ? m_scheduler->globalMinTimeMs() : 0;
}

qint64 TimelineModel::totalMaxMs() const {
    return m_scheduler ? m_scheduler->globalMaxTimeMs() : 0;
}

QString TimelineModel::viewSpanText() const {
    const qint64 sec = std::max<qint64>(1, (m_viewEndMs - m_viewStartMs) / 1000);
    if (sec >= 86400) return QStringLiteral("%1天").arg(static_cast<double>(sec) / 86400.0, 0, 'f', 1);
    if (sec >= 3600)  return QStringLiteral("%1小时").arg(static_cast<double>(sec) / 3600.0, 0, 'f', 1);
    if (sec >= 60)    return QStringLiteral("%1分钟").arg(sec / 60);
    return QStringLiteral("%1秒").arg(sec);
}

void TimelineModel::refreshFromScheduler() {
    if (!m_scheduler) return;
    const auto days = m_scheduler->availableDayStartsMs(m_tzOffsetSec);
    m_availableDays.clear();
    for (int64_t ds : days) {
        m_availableDays.append(TimeUtils::formatDateOnlyMs(ds, m_tzOffsetSec));
    }
    if (m_viewEndMs <= m_viewStartMs) {
        const qint64 maxT = totalMaxMs();
        if (maxT > 0) {
            fitFullDay(maxT);
        }
    }
    emit indexChanged();
}

void TimelineModel::clampView() {
    if (m_viewEndMs - m_viewStartMs < kMinSpanMs) {
        const qint64 mid = (m_viewStartMs + m_viewEndMs) / 2;
        m_viewStartMs = mid - kMinSpanMs / 2;
        m_viewEndMs = mid + kMinSpanMs / 2;
    } else if (m_viewEndMs - m_viewStartMs > kMaxSpanMs) {
        const qint64 mid = (m_viewStartMs + m_viewEndMs) / 2;
        m_viewStartMs = mid - kMaxSpanMs / 2;
        m_viewEndMs = mid + kMaxSpanMs / 2;
    }
}

void TimelineModel::zoomAt(double factor, double anchorRatio) {
    if (m_viewEndMs <= m_viewStartMs) return;
    const double ratio = std::clamp(anchorRatio, 0.0, 1.0);
    const qint64 oldSpan = m_viewEndMs - m_viewStartMs;
    const qint64 anchorMs = m_viewStartMs + static_cast<qint64>(std::llround(oldSpan * ratio));
    const qint64 newSpan = std::clamp(static_cast<qint64>(std::llround(oldSpan * factor)), kMinSpanMs, kMaxSpanMs);
    m_viewStartMs = anchorMs - static_cast<qint64>(std::llround(newSpan * ratio));
    m_viewEndMs = m_viewStartMs + newSpan;
    clampView();
    emit rangeChanged();
}

void TimelineModel::panByRatio(double deltaRatio) {
    if (m_viewEndMs <= m_viewStartMs) return;
    const qint64 delta = static_cast<qint64>(std::llround((m_viewEndMs - m_viewStartMs) * deltaRatio));
    m_viewStartMs += delta;
    m_viewEndMs += delta;
    emit rangeChanged();
}

void TimelineModel::fitFullDay(qint64 anyTimeInDayMs) {
    const int64_t ds = TimeUtils::dayStartMs(anyTimeInDayMs, m_tzOffsetSec);
    m_viewStartMs = ds;
    m_viewEndMs = ds + 86400LL * 1000LL;
    emit rangeChanged();
}

void TimelineModel::fitDateString(const QString& yyyyMmDd) {
    const auto ms = TimeUtils::parseDateTimeString(yyyyMmDd + QStringLiteral(" 00:00:00"), m_tzOffsetSec);
    if (ms.has_value()) {
        fitFullDay(*ms);
    }
}

void TimelineModel::setSpanSeconds(qint64 spanSec, qint64 centerMs) {
    const qint64 half = std::clamp(spanSec * 1000LL, kMinSpanMs, kMaxSpanMs) / 2;
    m_viewStartMs = centerMs - half;
    m_viewEndMs = centerMs + half;
    clampView();
    emit rangeChanged();
}

void TimelineModel::ensureTimeVisible(qint64 targetMs) {
    if (m_viewEndMs <= m_viewStartMs) {
        fitFullDay(targetMs);
        return;
    }
    const qint64 span = m_viewEndMs - m_viewStartMs;
    if (targetMs < m_viewStartMs || targetMs > m_viewEndMs) {
        m_viewStartMs = targetMs - span / 2;
        m_viewEndMs = m_viewStartMs + span;
        emit rangeChanged();
    }
}

namespace {
void appendDayNightSplitBars(QVariantList& out,
                             int64_t startMs,
                             int64_t endMs,
                             SegmentStatus status,
                             int64_t viewStartMs,
                             double span,
                             int tzOffsetSec) {
    if (endMs <= startMs || span <= 0.0) return;
    int64_t cur = startMs;
    while (cur < endMs) {
        const int64_t ds = TimeUtils::dayStartMs(cur, tzOffsetSec);
        const int64_t nextDs = ds + 86400LL * 1000LL;
        const DaySunTimes sun = SunTimesService::instance().getForDayStart(ds);

        const int64_t dayEnd = std::min(endMs, nextDs);
        if (cur < sun.sunriseMs) {
            const int64_t segEnd = std::min(dayEnd, sun.sunriseMs);
            if (segEnd > cur) {
                QVariantMap m;
                m.insert(QStringLiteral("x0"), static_cast<double>(cur - viewStartMs) / span);
                m.insert(QStringLiteral("x1"), static_cast<double>(segEnd - viewStartMs) / span);
                m.insert(QStringLiteral("status"), static_cast<int>(status));
                m.insert(QStringLiteral("isDay"), false);
                out.append(m);
                cur = segEnd;
            }
        }
        if (cur < dayEnd && cur < sun.sunsetMs) {
            const int64_t segEnd = std::min(dayEnd, sun.sunsetMs);
            if (segEnd > cur) {
                QVariantMap m;
                m.insert(QStringLiteral("x0"), static_cast<double>(cur - viewStartMs) / span);
                m.insert(QStringLiteral("x1"), static_cast<double>(segEnd - viewStartMs) / span);
                m.insert(QStringLiteral("status"), static_cast<int>(status));
                m.insert(QStringLiteral("isDay"), true);
                out.append(m);
                cur = segEnd;
            }
        }
        if (cur < dayEnd) {
            QVariantMap m;
            m.insert(QStringLiteral("x0"), static_cast<double>(cur - viewStartMs) / span);
            m.insert(QStringLiteral("x1"), static_cast<double>(dayEnd - viewStartMs) / span);
            m.insert(QStringLiteral("status"), static_cast<int>(status));
            m.insert(QStringLiteral("isDay"), false);
            out.append(m);
            cur = dayEnd;
        }
    }
}
} // namespace

QVariantList TimelineModel::getStreamBars(int streamIndex) const {
    QVariantList out;
    if (!m_scheduler || m_viewEndMs <= m_viewStartMs) return out;
    const double span = static_cast<double>(m_viewEndMs - m_viewStartMs);
    const auto intervals = m_scheduler->intervalsInRange(streamIndex, m_viewStartMs, m_viewEndMs);
    out.reserve(intervals.size() * 2);
    for (const auto& iv : intervals) {
        appendDayNightSplitBars(out, iv.startMs, iv.endMs, iv.status, m_viewStartMs, span, m_tzOffsetSec);
    }
    return out;
}

QVariantList TimelineModel::getCombinedBars() const {
    QVariantList out;
    if (!m_scheduler || m_viewEndMs <= m_viewStartMs) return out;
    const int n = m_scheduler->streamCount();
    QVector<TimeInterval> all;
    for (int i = 0; i < n; ++i) {
        const auto sub = m_scheduler->intervalsInRange(i, m_viewStartMs, m_viewEndMs);
        for (const auto& iv : sub) {
            if (iv.status == SegmentStatus::Ok || iv.status == SegmentStatus::Writing) {
                all.append(iv);
            }
        }
    }
    if (all.isEmpty()) return out;
    std::sort(all.begin(), all.end(), [](const TimeInterval& a, const TimeInterval& b) {
        return a.startMs < b.startMs;
    });

    QVector<TimeInterval> merged;
    merged.append(all.first());
    for (int i = 1; i < all.size(); ++i) {
        if (all[i].startMs <= merged.last().endMs + 2000) {
            merged.last().endMs = std::max(merged.last().endMs, all[i].endMs);
        } else {
            merged.append(all[i]);
        }
    }
    const double span = static_cast<double>(m_viewEndMs - m_viewStartMs);
    for (const auto& iv : merged) {
        appendDayNightSplitBars(out, iv.startMs, iv.endMs, SegmentStatus::Ok, m_viewStartMs, span, m_tzOffsetSec);
    }
    return out;
}

QList<QPointF> TimelineModel::nightBands() const {
    QList<QPointF> bands;
    if (m_viewEndMs <= m_viewStartMs) return bands;
    const double span = m_viewEndMs - m_viewStartMs;
    const auto append = [this, span, &bands](qint64 start, qint64 end) {
        start = std::max(start, m_viewStartMs);
        end = std::min(end, m_viewEndMs);
        if (end > start) {
            bands.append(QPointF((start - m_viewStartMs) / span, (end - m_viewStartMs) / span));
        }
    };
    constexpr qint64 dayMs = 86400 * 1000LL;
    for (qint64 day = TimeUtils::dayStartMs(m_viewStartMs, m_tzOffsetSec);
         day < m_viewEndMs; day += dayMs) {
        const auto sun = SunTimesService::instance().getForDayStart(day);
        append(day, sun.sunriseMs);
        append(sun.sunsetMs, day + dayMs);
    }
    return bands;
}

QVariantList TimelineModel::getTicks(int approxCount) const {
    QVariantList out;
    if (m_viewEndMs <= m_viewStartMs) return out;
    const qint64 spanMs = m_viewEndMs - m_viewStartMs;
    const qint64 targetStepMs = std::max<qint64>(1000, spanMs / std::max(2, approxCount));

    static constexpr qint64 kNiceSteps[] = {
        1000, 5000, 10000, 15000, 30000,
        60000, 120000, 300000, 600000, 900000, 1800000,
        3600000, 7200000, 14400000, 21600000, 43200000, 86400000
    };
    qint64 step = kNiceSteps[std::size(kNiceSteps) - 1];
    for (qint64 s : kNiceSteps) {
        if (s >= targetStepMs) {
            step = s;
            break;
        }
    }

    const qint64 tzOffMs = static_cast<qint64>(m_tzOffsetSec) * 1000LL;
    const qint64 firstTickLocal = ((m_viewStartMs + tzOffMs + step - 1) / step) * step;
    const qint64 firstTick = firstTickLocal - tzOffMs;
    const bool showDate = spanMs > 86400LL * 1000LL;

    for (qint64 t = firstTick; t <= m_viewEndMs; t += step) {
        const double ratio = static_cast<double>(t - m_viewStartMs) / static_cast<double>(spanMs);
        QVariantMap m;
        m.insert(QStringLiteral("ratio"), ratio);
        m.insert(QStringLiteral("label"), showDate
            ? TimeUtils::formatEpochMs(t, m_tzOffsetSec).mid(5, 11)
            : TimeUtils::formatTimeOnlyMs(t, m_tzOffsetSec));
        out.append(m);
    }
    return out;
}

} // namespace mcs
