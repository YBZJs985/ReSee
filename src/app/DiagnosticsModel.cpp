#include "app/DiagnosticsModel.h"
#include "cache/NasBlockCache.h"
#include "core/Logger.h"
#include "core/TimeUtils.h"
#include "sync/SyncScheduler.h"

#include <QFileInfo>
#include <algorithm>
#include <cmath>

#ifdef SYNC_PLATFORM_WINDOWS
#include <windows.h>
#include <psapi.h>
#elif defined(SYNC_PLATFORM_MACOS)
#include <mach/mach.h>
#include <unistd.h>
#endif

namespace mcs {

DiagnosticsModel::DiagnosticsModel(SyncScheduler* scheduler, NasBlockCache* cache, int tzOffsetSec, QObject* parent)
    : QAbstractListModel(parent), m_scheduler(scheduler), m_cache(cache), m_tzOffsetSec(tzOffsetSec) {}

int DiagnosticsModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : m_rows.size();
}

QVariant DiagnosticsModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size()) return {};
    const auto& d = m_rows.at(index.row());
    switch (role) {
    case StreamIndexRole: return d.streamIndex;
    case DisplayNameRole: return d.displayName;
    case StateTextRole:
        if (d.state == DecodeState::Buffering && d.waitDurationMs >= 500) {
            return QStringLiteral("缓冲中 %1s").arg(static_cast<double>(d.waitDurationMs) / 1000.0, 0, 'f', 1);
        }
        return decodeStateLabel(d.state);
    case StateCodeRole: return static_cast<int>(d.state);
    case WallTimeTextRole:
        if (d.state == DecodeState::NoVideo || d.state == DecodeState::Corrupt ||
            d.state == DecodeState::Disconnected || d.state == DecodeState::Idle) {
            return QStringLiteral("—");
        }
        if (d.hasValidTime && d.currentWallTimeMs > 0) {
            return TimeUtils::formatTimeOnlyMs(d.currentWallTimeMs, m_tzOffsetSec);
        }
        if (d.currentWallTimeMs > 0 && (d.state == DecodeState::Buffering || d.state == DecodeState::Locating || d.state == DecodeState::Preview)) {
            return TimeUtils::formatTimeOnlyMs(d.currentWallTimeMs, m_tzOffsetSec);
        }
        return QStringLiteral("—");
    case SyncErrorMsRole: return static_cast<qint64>(d.syncErrorMs);
    case SyncErrorTextRole:
        if (!d.hasValidTime) return QStringLiteral("—");
        return QStringLiteral("%1ms").arg(d.syncErrorMs);
    case HasValidTimeRole: return d.hasValidTime;
    case WaitMsRole: return static_cast<qint64>(d.waitDurationMs);
    case FpsRole: return d.decodedFps;
    case QueueDepthRole: return d.queueDepth;
    case HwBackendRole: return d.hwBackend;
    case OffsetMsRole: return static_cast<qint64>(d.appliedOffsetMs);
    case CalibConfidenceRole: return d.calibConfidence;
    case SeekMsRole: return d.lastSeekMs;
    case NasReadMsRole: return d.lastNasReadMs;
    case ResolutionRole:
        return (d.width > 0 && d.height > 0) ? QStringLiteral("%1×%2").arg(d.width).arg(d.height) : QStringLiteral("-");
    case CurrentFileRole:
        return d.currentFile.isEmpty() ? QStringLiteral("-") : QFileInfo(d.currentFile).fileName();
    case ErrorMsgRole: return d.errorMessage;
    }
    return {};
}

QHash<int, QByteArray> DiagnosticsModel::roleNames() const {
    return {
        {StreamIndexRole, "streamIndex"},
        {DisplayNameRole, "displayName"},
        {StateTextRole, "stateText"},
        {StateCodeRole, "stateCode"},
        {WallTimeTextRole, "wallTimeText"},
        {SyncErrorMsRole, "syncErrorMs"},
        {SyncErrorTextRole, "syncErrorText"},
        {HasValidTimeRole, "hasValidTime"},
        {WaitMsRole, "waitDurationMs"},
        {FpsRole, "fps"},
        {QueueDepthRole, "queueDepth"},
        {HwBackendRole, "hwBackend"},
        {OffsetMsRole, "offsetMs"},
        {CalibConfidenceRole, "calibConfidence"},
        {SeekMsRole, "seekMs"},
        {NasReadMsRole, "nasReadMs"},
        {ResolutionRole, "resolution"},
        {CurrentFileRole, "currentFile"},
        {ErrorMsgRole, "errorMessage"}
    };
}

void DiagnosticsModel::refresh() {
    if (!m_scheduler) return;
    const auto latest = m_scheduler->allDiagnostics();
    const bool sizeChanged = (latest.size() != m_rows.size());
    if (sizeChanged) {
        beginResetModel();
        m_rows = latest;
        endResetModel();
    } else {
        m_rows = latest;
        if (!m_rows.isEmpty()) {
            emit dataChanged(index(0), index(m_rows.size() - 1));
        }
    }

    qint64 maxErr = 0;
    bool hasValidErr = false;
    int synced = 0;
    int noVid = 0;
    int notReady = 0;
    bool anyLocating = false;
    bool anyBuffering = false;
    bool anyError = false;

    for (const auto& d : m_rows) {
        if (d.state == DecodeState::NoVideo) {
            ++noVid;
            continue;
        }
        if (d.state == DecodeState::Locating) {
            anyLocating = true;
            ++notReady;
        } else if (d.state == DecodeState::Buffering) {
            anyBuffering = true;
            ++notReady;
        } else if (d.state == DecodeState::Corrupt || d.state == DecodeState::Disconnected) {
            anyError = true;
            ++notReady;
        } else if (d.state == DecodeState::Idle) {
            ++notReady;
        } else if (d.state == DecodeState::Playing && d.hasValidTime) {
            maxErr = std::max<qint64>(maxErr, std::abs(d.syncErrorMs));
            hasValidErr = true;
            if (std::abs(d.syncErrorMs) < 1000) {
                ++synced;
            }
        }
    }
    m_maxSyncErrorMs = maxErr;
    m_hasValidSyncError = hasValidErr;
    m_syncedCount = synced;
    m_noVideoCount = noVid;
    m_notReadyCount = notReady;

    const auto* clk = m_scheduler->clock();
    if (clk && clk->isScrubbing()) {
        m_globalStateText = QStringLiteral("极速穿梭");
        m_globalStateCode = 2;
    } else if (clk && !clk->isPlaying()) {
        m_globalStateText = anyLocating ? QStringLiteral("定位中") : QStringLiteral("暂停");
        m_globalStateCode = anyLocating ? 3 : 1;
    } else if (anyLocating) {
        m_globalStateText = QStringLiteral("定位中");
        m_globalStateCode = 3;
    } else if (anyBuffering) {
        m_globalStateText = QStringLiteral("缓冲中");
        m_globalStateCode = 4;
    } else if (anyError) {
        m_globalStateText = QStringLiteral("部分异常");
        m_globalStateCode = 5;
    } else if (noVid == m_rows.size() && !m_rows.isEmpty()) {
        m_globalStateText = QStringLiteral("无录像");
        m_globalStateCode = 6;
    } else {
        m_globalStateText = QStringLiteral("正常播放");
        m_globalStateCode = 0;
    }

    if (m_cache) {
        const auto st = m_cache->stats();
        m_memCacheMb = static_cast<double>(st.memBytesUsed) / (1024.0 * 1024.0);
        m_memCacheMaxMb = static_cast<double>(st.memMaxBytes) / (1024.0 * 1024.0);
        m_ssdCacheGb = static_cast<double>(st.ssdBytesUsed) / (1024.0 * 1024.0 * 1024.0);
        m_ssdCacheMaxGb = static_cast<double>(st.ssdMaxBytes) / (1024.0 * 1024.0 * 1024.0);
        const uint64_t totalReqs = st.memHits + st.ssdHits + st.nasReads;
        m_cacheHitRate = totalReqs > 0 ? (100.0 * static_cast<double>(st.memHits + st.ssdHits) / static_cast<double>(totalReqs)) : 0.0;
        m_avgNasReadMs = st.avgNasReadMs;
        m_nasDisconnected = st.nasDisconnected;
    }

    sampleSystemUsage();
    emit summaryChanged();
}

void DiagnosticsModel::sampleSystemUsage() {
#ifdef SYNC_PLATFORM_WINDOWS
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
        m_processMemMb = static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0);
    }

    FILETIME ftCreate{}, ftExit{}, ftKernel{}, ftUser{};
    if (GetProcessTimes(GetCurrentProcess(), &ftCreate, &ftExit, &ftKernel, &ftUser)) {
        auto toU64 = [](const FILETIME& f) {
            return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
        };
        const uint64_t k = toU64(ftKernel);
        const uint64_t u = toU64(ftUser);
        const uint64_t now = static_cast<uint64_t>(TimeUtils::nowEpochMs());
        if (m_prevWallTime > 0 && now > m_prevWallTime) {
            const double cpuDelta100ns = static_cast<double>((k - m_prevCpuKernel) + (u - m_prevCpuUser));
            const double wallDelta100ns = static_cast<double>(now - m_prevWallTime) * 10000.0;
            SYSTEM_INFO si{};
            GetSystemInfo(&si);
            const double cores = std::max(1.0, static_cast<double>(si.dwNumberOfProcessors));
            m_cpuUsagePct = std::clamp((cpuDelta100ns / (wallDelta100ns * cores)) * 100.0, 0.0, 100.0);
        }
        m_prevCpuKernel = k;
        m_prevCpuUser = u;
        m_prevWallTime = now;
    }
#elif defined(SYNC_PLATFORM_MACOS)
    mach_task_basic_info info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
        m_processMemMb = static_cast<double>(info.resident_size) / (1024.0 * 1024.0);
    }
    task_thread_times_info_data_t tinfo{};
    mach_msg_type_number_t tcount = TASK_THREAD_TIMES_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_THREAD_TIMES_INFO, reinterpret_cast<task_info_t>(&tinfo), &tcount) == KERN_SUCCESS) {
        const uint64_t userUs = static_cast<uint64_t>(tinfo.user_time.seconds) * 1000000ULL +
                                static_cast<uint64_t>(tinfo.user_time.microseconds);
        const uint64_t sysUs = static_cast<uint64_t>(tinfo.system_time.seconds) * 1000000ULL +
                               static_cast<uint64_t>(tinfo.system_time.microseconds);
        const uint64_t now = static_cast<uint64_t>(TimeUtils::nowEpochMs());
        if (m_prevWallTime > 0 && now > m_prevWallTime) {
            const double cpuDeltaUs = static_cast<double>((userUs - m_prevCpuUser) + (sysUs - m_prevCpuKernel));
            const double wallDeltaUs = static_cast<double>(now - m_prevWallTime) * 1000.0;
            const long ncpu = std::max<long>(1, sysconf(_SC_NPROCESSORS_ONLN));
            m_cpuUsagePct = std::clamp((cpuDeltaUs / (wallDeltaUs * static_cast<double>(ncpu))) * 100.0, 0.0, 100.0);
        }
        m_prevCpuUser = userUs;
        m_prevCpuKernel = sysUs;
        m_prevWallTime = now;
    }
#endif
}

QStringList DiagnosticsModel::recentLogs() const {
    return Logger::instance().recentLines(40);
}

} // namespace mcs
