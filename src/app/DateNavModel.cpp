#include "app/DateNavModel.h"
#include "core/SunTimes.h"
#include "core/TimeUtils.h"
#include "sync/SyncScheduler.h"

#include <QDate>
#include <QSet>
#include <algorithm>
#include <cmath>

namespace mcs {

namespace {
QString weekdayLabel(int y, int m, int d) {
    static constexpr const char* kNames[] = {"", "周一", "周二", "周三", "周四", "周五", "周六", "周日"};
    const QDate qd(y, m, d);
    const int dow = qd.isValid() ? qd.dayOfWeek() : 0;
    if (dow >= 1 && dow <= 7) {
        return QString::fromUtf8(kNames[dow]);
    }
    return QString();
}
} // namespace

DateNavModel::DateNavModel(SyncScheduler* scheduler, int tzOffsetSec, QObject* parent)
    : QAbstractListModel(parent), m_scheduler(scheduler), m_tzOffsetSec(tzOffsetSec) {}

int DateNavModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : m_rows.size();
}

QVariant DateNavModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size()) return {};
    const auto& e = m_rows.at(index.row());
    switch (role) {
    case DateStrRole: return e.dateStr;
    case MonthDayRole: return e.monthDayText;
    case YearRole: return e.yearText;
    case WeekdayRole: return e.weekdayText;
    case DayStartMsRole: return e.dayStartMs;
    case HasVideoRole: return e.hasVideo;
    case IsTodayRole: return e.isToday;
    case MaxStreamsRole: return e.maxStreams;
    case SunriseFracRole: return e.sunriseFrac;
    case SunsetFracRole: return e.sunsetFrac;
    case CoverageSlotsRole: return e.coverageSlots;
    case IsSelectedRole: return e.dateStr == m_selectedDateStr;
    }
    return {};
}

QHash<int, QByteArray> DateNavModel::roleNames() const {
    return {
        {DateStrRole, "dateStr"},
        {MonthDayRole, "monthDayText"},
        {YearRole, "yearText"},
        {WeekdayRole, "weekdayText"},
        {DayStartMsRole, "dayStartMs"},
        {HasVideoRole, "hasVideo"},
        {IsTodayRole, "isToday"},
        {MaxStreamsRole, "maxStreams"},
        {SunriseFracRole, "sunriseFrac"},
        {SunsetFracRole, "sunsetFrac"},
        {CoverageSlotsRole, "coverageSlots"},
        {IsSelectedRole, "isSelected"}
    };
}

void DateNavModel::refreshFromScheduler() {
    if (!m_scheduler) return;

    const int64_t todayStart = TimeUtils::dayStartMs(TimeUtils::nowEpochMs(), m_tzOffsetSec);
    const auto videoDays = m_scheduler->availableDayStartsMs(m_tzOffsetSec);

    QSet<int64_t> allDayStarts;
    for (int64_t ds : videoDays) {
        allDayStarts.insert(ds);
    }
    allDayStarts.insert(todayStart);
    allDayStarts.insert(todayStart - 86400LL * 1000LL);

    if (!videoDays.isEmpty()) {
        const int64_t minDs = videoDays.front();
        const int64_t maxDs = std::max(videoDays.back(), todayStart);
        // 补齐有录像首日至今日之间的空白天，最多跨度 180 天防极端错误文件名爆表
        const int64_t clampedMin = std::max(minDs, maxDs - 180LL * 86400LL * 1000LL);
        for (int64_t ds = clampedMin; ds <= maxDs; ds += 86400LL * 1000LL) {
            allDayStarts.insert(ds);
        }
    }

    QVector<int64_t> sorted = allDayStarts.values().toVector();
    // 倒序排列：最新日期在最上方
    std::sort(sorted.begin(), sorted.end(), std::greater<int64_t>());

    QVector<DateNavEntry> nextAll;
    nextAll.reserve(sorted.size());

    for (int64_t ds : sorted) {
        DateNavEntry e;
        e.dayStartMs = ds;
        e.isToday = (ds == todayStart);

        int y = 0, m = 0, d = 0, h = 0, mi = 0, s = 0, ms = 0;
        TimeUtils::epochMsToCivil(ds, y, m, d, h, mi, s, ms, m_tzOffsetSec);
        e.dateStr = QStringLiteral("%1-%2-%3")
                        .arg(y, 4, 10, QLatin1Char('0'))
                        .arg(m, 2, 10, QLatin1Char('0'))
                        .arg(d, 2, 10, QLatin1Char('0'));
        e.monthDayText = QStringLiteral("%1.%2")
                             .arg(m, 2, 10, QLatin1Char('0'))
                             .arg(d, 2, 10, QLatin1Char('0'));
        e.yearText = QString::number(y);
        e.weekdayText = weekdayLabel(y, m, d);

        const QVector<int> covSlots = m_scheduler->dayCoverageSlots(ds, 96);
        e.coverageSlots.reserve(covSlots.size());
        int maxSt = 0;
        for (int cnt : covSlots) {
            e.coverageSlots.append(cnt);
            maxSt = std::max(maxSt, cnt);
        }
        e.maxStreams = maxSt;
        e.hasVideo = (maxSt > 0);
        const DaySunTimes sun = SunTimesService::instance().getForDayStart(ds);
        e.sunriseFrac = sun.sunriseFrac;
        e.sunsetFrac = sun.sunsetFrac;
        nextAll.append(std::move(e));
    }

    m_allEntries = std::move(nextAll);
    rebuildFilteredRows();
}

void DateNavModel::rebuildFilteredRows() {
    const int64_t todayStart = TimeUtils::dayStartMs(TimeUtils::nowEpochMs(), m_tzOffsetSec);
    const int64_t min7DayStart = todayStart - 6LL * 86400LL * 1000LL;

    beginResetModel();
    m_rows.clear();
    for (const auto& e : m_allEntries) {
        if (m_recent7DaysOnly && (e.dayStartMs < min7DayStart || e.dayStartMs > todayStart)) {
            continue;
        }
        // 允许保留当前选中日，即使该日全天无录像也明确高亮显示
        if (m_onlyWithVideo && !e.hasVideo && e.dateStr != m_selectedDateStr) {
            continue;
        }
        m_rows.append(e);
    }
    endResetModel();

    updateSelectionRow();
    emit listChanged();
}

void DateNavModel::updateMasterTime(int64_t masterWallMs) {
    if (masterWallMs <= 0) return;

    const QString newDateStr = TimeUtils::formatDateOnlyMs(masterWallMs, m_tzOffsetSec);
    const int64_t ds = TimeUtils::dayStartMs(masterWallMs, m_tzOffsetSec);
    const double frac = std::clamp(static_cast<double>(masterWallMs - ds) / 86400000.0, 0.0, 1.0);

    const bool dateChanged = (newDateStr != m_selectedDateStr);
    const bool fracChanged = std::abs(frac - m_currentDayFraction) > 0.001;
    if (!dateChanged && !fracChanged) return;

    const QString prevSelected = m_selectedDateStr;
    m_selectedDateStr = newDateStr;
    m_currentDayFraction = frac;

    if (dateChanged) {
        if (m_onlyWithVideo) {
            // 若新选中日期是空天或先前选中了空天，重建列表确保选中行始终可见
            bool foundNew = false;
            for (const auto& e : m_rows) {
                if (e.dateStr == newDateStr) {
                    foundNew = true;
                    break;
                }
            }
            bool hadEmptyPrev = false;
            for (const auto& e : m_rows) {
                if (e.dateStr == prevSelected && !e.hasVideo) {
                    hadEmptyPrev = true;
                    break;
                }
            }
            if (!foundNew || hadEmptyPrev) {
                rebuildFilteredRows();
                return;
            }
        }
        updateSelectionRow();
        if (!m_rows.isEmpty()) {
            emit dataChanged(index(0), index(m_rows.size() - 1), {IsSelectedRole});
        }
    }
    emit selectionChanged();
}

void DateNavModel::updateSelectionRow() {
    int found = -1;
    for (int i = 0; i < m_rows.size(); ++i) {
        if (m_rows[i].dateStr == m_selectedDateStr) {
            found = i;
            break;
        }
    }
    m_selectedRowIndex = found;
}

void DateNavModel::setOnlyWithVideo(bool v) {
    if (m_onlyWithVideo == v) return;
    m_onlyWithVideo = v;
    emit filterChanged();
    rebuildFilteredRows();
}

void DateNavModel::setRecent7DaysOnly(bool v) {
    if (m_recent7DaysOnly == v) return;
    m_recent7DaysOnly = v;
    emit filterChanged();
    rebuildFilteredRows();
}

QString DateNavModel::hoverInfoForSlot(int rowIndex, double fraction) const {
    if (rowIndex < 0 || rowIndex >= m_rows.size()) return QString();
    const auto& e = m_rows.at(rowIndex);
    const double f = std::clamp(fraction, 0.0, 0.9999);
    const int totalMin = std::clamp(static_cast<int>(std::floor(f * 1440.0)), 0, 1439);
    const int hh = totalMin / 60;
    const int mm = totalMin % 60;
    const int slotIdx = std::clamp(static_cast<int>(std::floor(f * static_cast<double>(e.coverageSlots.size()))),
                                   0, std::max(0, static_cast<int>(e.coverageSlots.size()) - 1));
    const int streamCnt = (slotIdx >= 0 && slotIdx < e.coverageSlots.size()) ? e.coverageSlots.at(slotIdx).toInt() : 0;

    const QString timeStr = QStringLiteral("%1:%2")
                                .arg(hh, 2, 10, QLatin1Char('0'))
                                .arg(mm, 2, 10, QLatin1Char('0'));
    const QString covStr = (streamCnt > 0)
                               ? QStringLiteral("%1/%2 路有录像").arg(streamCnt).arg(m_scheduler ? m_scheduler->streamCount() : 0)
                               : QStringLiteral("无录像");
    return QStringLiteral("%1.%2 %3 · %4").arg(e.yearText, e.monthDayText, timeStr, covStr);
}

double DateNavModel::coverage(const QString& date) const {
    for (const auto& entry : m_allEntries) {
        if (entry.dateStr != date || entry.coverageSlots.isEmpty()) continue;
        int covered = 0;
        for (const auto& slot : entry.coverageSlots) {
            if (slot.toInt() > 0) ++covered;
        }
        return static_cast<double>(covered) / entry.coverageSlots.size();
    }
    return 0.0;
}

bool DateNavModel::hasGap(const QString& date) const {
    const auto fraction = coverage(date);
    return fraction > 0.0 && fraction < 1.0;
}

QString DateNavModel::todayDateStr() const {
    return TimeUtils::formatDateOnlyMs(TimeUtils::nowEpochMs(), m_tzOffsetSec);
}

QString DateNavModel::yesterdayDateStr() const {
    return TimeUtils::formatDateOnlyMs(TimeUtils::nowEpochMs() - 86400LL * 1000LL, m_tzOffsetSec);
}

} // namespace mcs
