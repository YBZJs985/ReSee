#include "app/PlaybackController.h"
#include "app/BookmarkModel.h"
#include "app/DateNavModel.h"
#include "app/DiagnosticsModel.h"
#include "app/HealthModel.h"
#include "app/TimelineModel.h"
#include "cache/NasBlockCache.h"
#include "core/Logger.h"
#include "core/SunTimes.h"
#include "core/TimeUtils.h"
#include "health/HealthMonitor.h"
#include "index/FileScanner.h"
#include "index/IndexDatabase.h"
#include "render/VideoRenderItem.h"
#include "sync/SyncScheduler.h"

#include <QDate>
#include <QTime>
#include <QRegularExpression>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QImage>
#include <QKeySequence>
#include <QVariantMap>
#include <algorithm>
#include <cmath>

namespace mcs {

namespace {
QImage nv12FrameToQImage(const DecodedFrame& f) {
    if (!f.nv12Data || f.width <= 0 || f.height <= 0) return {};
    const int w = f.width;
    const int h = f.height;
    const size_t yBytes = static_cast<size_t>(f.yStride) * h;
    const size_t uvBytes = static_cast<size_t>(f.uvStride) * ((h + 1) / 2);
    if (f.nv12Data->size() < yBytes + uvBytes) return {};

    QImage img(w, h, QImage::Format_RGB888);
    const uint8_t* yPlane = f.nv12Data->data();
    const uint8_t* uvPlane = yPlane + yBytes;

    for (int y = 0; y < h; ++y) {
        const uint8_t* yRow = yPlane + static_cast<size_t>(y) * f.yStride;
        const uint8_t* uvRow = uvPlane + static_cast<size_t>(y / 2) * f.uvStride;
        uint8_t* dst = img.scanLine(y);
        for (int x = 0; x < w; ++x) {
            const int c = std::max(0, static_cast<int>(yRow[x]) - 16);
            const int uvIdx = (x / 2) * 2;
            const int d = static_cast<int>(uvRow[uvIdx]) - 128;
            const int e = static_cast<int>(uvRow[uvIdx + 1]) - 128;
            const int yScaled = 298 * c + 128;
            const int r = std::clamp((yScaled + 459 * e) >> 8, 0, 255);
            const int g = std::clamp((yScaled - 55 * d - 136 * e) >> 8, 0, 255);
            const int b = std::clamp((yScaled + 541 * d) >> 8, 0, 255);
            dst[x * 3 + 0] = static_cast<uint8_t>(r);
            dst[x * 3 + 1] = static_cast<uint8_t>(g);
            dst[x * 3 + 2] = static_cast<uint8_t>(b);
        }
    }
    return img;
}
} // namespace

PlaybackController::PlaybackController(QObject* parent) : QObject(parent) {
    m_config = AppConfig::createDefault();
    m_firstRun = !QFileInfo::exists(m_config.configFilePath());
    m_config.loadFromJson(m_config.configFilePath());
    if (m_config.lastFocusedStream >= streamCount()
        || (m_config.lastFocusedStream == -2 && !groupedLayoutAvailable())) {
        m_config.lastFocusedStream = -1;
    }
    SunTimesService::instance().configure(m_config.locationConfigured, m_config.latitude,
                                          m_config.longitude, m_config.timezoneOffsetSec);
    Logger::instance().init(m_config.logDir);
    MCS_LOG_INFO("APP", QStringLiteral("ReSee 启动，数据目录: %1").arg(m_config.dataDir));

    m_db = std::make_unique<IndexDatabase>();
    m_db->open(m_config.dbPath);
    m_db->syncStreamsConfig(m_config.cameras, m_config.streams);

    m_cache = std::make_unique<NasBlockCache>();
    m_cache->configure(m_config.cacheDir, m_config.cacheMaxGb, m_config.memCacheMaxMb, m_config.maxConcurrentNasReads);

    m_scheduler = std::make_unique<SyncScheduler>(m_db.get(), m_cache.get());
    m_scanner = std::make_unique<FileScanner>(m_db.get(), m_cache.get());
    m_healthMonitor = std::make_unique<HealthMonitor>(m_db.get());
    m_timeline = new TimelineModel(m_scheduler.get(), m_config.timezoneOffsetSec, this);
    m_dateNav = new DateNavModel(m_scheduler.get(), m_config.timezoneOffsetSec, this);
    m_bookmarks = new BookmarkModel(m_db.get(), m_config.timezoneOffsetSec, this);
    m_diagnostics = new DiagnosticsModel(m_scheduler.get(), m_cache.get(), m_config.timezoneOffsetSec, this);
    m_healthModel = new HealthModel(m_healthMonitor.get(), m_config.timezoneOffsetSec, this);

    m_autoSaveTimer = new QTimer(this);
    m_autoSaveTimer->setInterval(10000);
    connect(m_autoSaveTimer, &QTimer::timeout, this, &PlaybackController::saveState);

    m_meterTimer = new QTimer(this);
    m_meterTimer->setInterval(33);
    connect(m_meterTimer, &QTimer::timeout, this, &PlaybackController::onAudioMeterTick);
}

PlaybackController::~PlaybackController() {
    if (m_autoSaveTimer) m_autoSaveTimer->stop();
    if (m_meterTimer) m_meterTimer->stop();
    for (auto& p : m_videoItems) p.clear();
    saveState();
    if (m_healthMonitor) m_healthMonitor->stop();
    if (m_scanner) m_scanner->stop();
    if (m_scheduler) m_scheduler->shutdown();
    if (m_db) m_db->close();
}

void PlaybackController::initialize() {
    m_scheduler->initialize(m_config);
    m_scheduler->setFrameSink([this](int streamIndex,
                                     const std::shared_ptr<const DecodedFrame>& frame,
                                     DecodeState state,
                                     int64_t waitDurationMs) {
        if (streamIndex >= 0 && streamIndex < m_videoItems.size()) {
            if (auto* item = m_videoItems[streamIndex].data()) {
                item->updateFrame(frame, state, m_config.timezoneOffsetSec, waitDurationMs);
            }
        }
    });

    connect(m_scheduler->clock(), &MasterClock::timeChanged, this, [this](int64_t ms) {
        if (m_dateNav) {
            m_dateNav->updateMasterTime(ms);
        }
        emit masterTimeChanged();
    });
    connect(m_scheduler->clock(), &MasterClock::stateChanged, this, [this](bool playing, double, bool scrubbing) {
        if (m_scanner) {
            m_scanner->setForegroundBusy(playing || scrubbing);
        }
        if (m_healthMonitor) {
            m_healthMonitor->setForegroundBusy(playing || scrubbing);
        }
        emit clockStateChanged();
    });
    connect(m_scheduler.get(), &SyncScheduler::audioStreamChanged, this, &PlaybackController::audioChanged);
    connect(m_scheduler.get(), &SyncScheduler::streamOffsetChanged, this, &PlaybackController::settingsChanged);
    connect(m_scheduler.get(), &SyncScheduler::diagnosticsUpdated, this, [this]() {
        m_diagnostics->refresh();
    });

    connect(m_scanner.get(), &FileScanner::progressChanged, this, [this](const ScanProgress& p) {
        if (p.scanning && !p.stageBRunning) m_scanIssues.clear();
        m_scanProgress = p;
        if (m_scheduler) {
            m_scheduler->setIndexingActive(m_scanner->isStageARunning());
        }
        emit scanChanged();
    });
    connect(m_scanner.get(), &FileScanner::stageACompleted, this, &PlaybackController::onStageACompleted);
    connect(m_scanner.get(), &FileScanner::stageBCompleted, this, &PlaybackController::onStageBCompleted);
    connect(m_scanner.get(), &FileScanner::cameraRescanCompleted, this, &PlaybackController::onCameraRescanCompleted);
    connect(m_scanner.get(), &FileScanner::scanError, this, &PlaybackController::reportScanIssue);

    connect(m_healthMonitor.get(), &HealthMonitor::newFilesDetectedOnCamera, this, [this](int streamIdx) {
        const int64_t now = TimeUtils::nowEpochMs();
        if (now - m_lastAutoRescanMs.value(streamIdx, 0) > 25000 && m_scanner) {
            m_lastAutoRescanMs.insert(streamIdx, now);
            m_scanner->startCameraRescan(m_config, streamIdx);
        }
    });

    // 恢复音量、静音和初始音源路由
    m_scheduler->audioPlayer()->setVolume(m_config.audioVolume);
    m_scheduler->audioPlayer()->setMuted(m_config.audioMuted);
    applyAudioRoutingForFocus(m_config.lastFocusedStream);

    m_bookmarks->reload();
    m_timeline->refreshFromScheduler();
    m_dateNav->refreshFromScheduler();
    m_diagnostics->refresh();

    // 若已有历史索引，立刻先恢复上次断点或定位到有效时刻，同时后台增量扫描新文件
    if (m_scheduler->globalMaxTimeMs() > 0) {
        pickInitialPlaybackTime();
    }

    m_autoSaveTimer->start();
    m_meterTimer->start();
    m_scheduler->setIndexingActive(true);
    m_scanner->startScan(m_config, true);
    m_healthMonitor->configure(m_config);
    m_healthMonitor->start();
}

void PlaybackController::pickInitialPlaybackTime() {
    if (m_initialSeekDone) return;
    const int64_t minT = m_scheduler->globalMinTimeMs();
    const int64_t maxT = m_scheduler->globalMaxTimeMs();
    if (maxT <= 0) return;

    int64_t targetMs = 0;
    if (m_config.lastMasterTimeMs >= minT && m_config.lastMasterTimeMs <= maxT) {
        targetMs = m_config.lastMasterTimeMs;
    } else {
        // 优先从最新录像前约 30 分钟恢复，短录像则从首段开始。
        targetMs = std::max(minT, maxT - int64_t{1800 * 1000});
        // 若已知双摄最后时间早于单摄，检查双摄最后段
        if (groupedLayoutAvailable() && !m_scheduler->segmentAt(0, targetMs)) {
            const auto days = m_scheduler->availableDayStartsMs(m_config.timezoneOffsetSec);
            if (!days.isEmpty()) {
                // 从有双摄数据的最后一天中午开始试探
                for (int i = days.size() - 1; i >= 0; --i) {
                    const int64_t noon = days[i] + 12LL * 3600 * 1000LL;
                    if (m_scheduler->segmentAt(0, noon) && m_scheduler->segmentAt(2, noon)) {
                        targetMs = noon;
                        break;
                    }
                }
            }
        }
    }

    m_initialSeekDone = true;
    m_scheduler->clock()->setInitialTime(targetMs, m_config.lastPlaybackRate > 0 ? m_config.lastPlaybackRate : 1.0);
    m_timeline->fitFullDay(targetMs);
    m_dateNav->updateMasterTime(targetMs);
    m_scheduler->seekTo(targetMs);
}

void PlaybackController::onStageACompleted(int total, int changed, int removed) {
    m_scheduler->setIndexingActive(false);
    m_scheduler->reloadIndex();
    m_timeline->refreshFromScheduler();
    m_dateNav->refreshFromScheduler();
    if (!m_initialSeekDone && total > 0) {
        pickInitialPlaybackTime();
    } else if (changed > 0 || removed > 0) {
        // 若当前时间点在扫描前处于索引中/空洞，新扫描出文件后立刻重新对齐画面
        if (masterTimeMs() > 0) {
            m_scheduler->seekTo(masterTimeMs());
        }
        emit toastMessage(QStringLiteral("索引更新完成：共 %1 个片段（新增/变更 %2）").arg(total).arg(changed), false);
    } else if (masterTimeMs() > 0) {
        m_scheduler->seekTo(masterTimeMs());
    }
}

void PlaybackController::onStageBCompleted(int probed, int corrupt) {
    if (probed > 0) {
        m_scheduler->reloadIndex();
        m_timeline->refreshFromScheduler();
        m_dateNav->refreshFromScheduler();
    }
    if (corrupt > 0) {
        reportScanIssue(QStringLiteral("%1 个录像片段无法读取。请确认录像写入已完成，并尝试用其他播放器打开这些文件。").arg(corrupt));
    }
}

void PlaybackController::onCameraRescanCompleted(int streamIndex, int changedCount) {
    m_scheduler->reloadIndex();
    m_timeline->refreshFromScheduler();
    m_dateNav->refreshFromScheduler();
    if (changedCount > 0 && masterTimeMs() > 0) {
        m_scheduler->seekTo(masterTimeMs());
        emit toastMessage(QStringLiteral("局部复查发现 %1 个更新片段，已同步定位").arg(changedCount), false);
    } else {
        Q_UNUSED(streamIndex);
    }
}

void PlaybackController::registerVideoItem(int streamIndex, QObject* itemObj) {
    auto* item = qobject_cast<VideoRenderItem*>(itemObj);
    if (!item) {
        MCS_LOG_WARN("UI", QStringLiteral("registerVideoItem 转换失败: stream=%1").arg(streamIndex));
        return;
    }
    if (streamIndex >= 0) {
        if (streamIndex >= m_videoItems.size()) {
            m_videoItems.resize(streamIndex + 1);
        }
        m_videoItems[streamIndex] = item;
        if (m_scheduler) {
            if (auto f = m_scheduler->latestFrame(streamIndex)) {
                const auto diags = m_scheduler->allDiagnostics();
                const DecodeState st = (streamIndex < diags.size()) ? diags[streamIndex].state : DecodeState::Playing;
                item->updateFrame(f, st, m_config.timezoneOffsetSec);
            }
        }
    }
}

void PlaybackController::unregisterVideoItem(int streamIndex, QObject* itemObj) {
    auto* item = qobject_cast<VideoRenderItem*>(itemObj);
    if (streamIndex >= 0 && streamIndex < m_videoItems.size()) {
        if (m_videoItems[streamIndex] == item) {
            m_videoItems[streamIndex] = nullptr;
        }
    }
}

bool PlaybackController::playing() const { return m_scheduler->clock()->isPlaying(); }
bool PlaybackController::scrubbing() const { return m_scheduler->clock()->isScrubbing(); }
double PlaybackController::playbackRate() const { return m_scheduler->clock()->playbackRate(); }
qint64 PlaybackController::masterTimeMs() const { return m_scheduler->clock()->currentTimeMs(); }

QString PlaybackController::masterDateText() const {
    const qint64 ms = masterTimeMs();
    return ms > 0 ? TimeUtils::formatDateOnlyMs(ms, m_config.timezoneOffsetSec) : QStringLiteral("---- -- --");
}

QString PlaybackController::masterTimeText() const {
    const qint64 ms = masterTimeMs();
    return ms > 0 ? TimeUtils::formatTimeOnlyMs(ms, m_config.timezoneOffsetSec) : QStringLiteral("--:--:--");
}

QString PlaybackController::masterFullText() const {
    const qint64 ms = masterTimeMs();
    return ms > 0 ? TimeUtils::formatEpochMs(ms, m_config.timezoneOffsetSec) : QStringLiteral("未加载录像");
}

QString PlaybackController::formatEpochMs(qint64 ms) const {
    return ms > 0 ? TimeUtils::formatEpochMs(ms, m_config.timezoneOffsetSec) : QString();
}

void PlaybackController::setLayoutMode(const QString& mode) {
    if (m_config.lastLayoutMode == mode) return;
    m_config.lastLayoutMode = mode;
    emit layoutChanged();
}

bool PlaybackController::groupedLayoutAvailable() const {
    return m_config.streams.size() == 6
        && m_config.streams[0].cameraId == m_config.streams[1].cameraId;
}

void PlaybackController::setFocusedStream(int idx) {
    if (idx < -2 || idx >= streamCount() || (idx == -2 && !groupedLayoutAvailable())) return;
    if (m_config.lastFocusedStream == idx) return;
    m_config.lastFocusedStream = idx;
    applyAudioRoutingForFocus(idx);
    emit layoutChanged();
}

void PlaybackController::applyAudioRoutingForFocus(int focusedIdx) {
    if (!m_scheduler) return;
    if (!m_config.audioFollowsFocus) {
        m_scheduler->selectAudioStream(m_config.manualGridAudioStream);
        return;
    }
    if (focusedIdx >= 0 && focusedIdx < m_config.streams.size()) {
        if (focusedIdx <= 1) {
            m_config.lastDualAudioStream = focusedIdx;
        }
        m_scheduler->selectAudioStream(focusedIdx);
    } else if (focusedIdx == -2) {
        const int dualIdx = (m_config.lastDualAudioStream == 1) ? 1 : 0;
        m_scheduler->selectAudioStream(dualIdx);
    } else {
        m_scheduler->selectAudioStream(m_config.manualGridAudioStream);
    }
}

int PlaybackController::audioStreamIndex() const {
    return m_scheduler->selectedAudioStream();
}

void PlaybackController::setTimelineExpanded(bool v) {
    if (m_config.timelineExpanded == v) return;
    m_config.timelineExpanded = v;
    emit layoutChanged();
}

void PlaybackController::setDiagnosticsVisible(bool v) {
    if (m_config.diagnosticsVisible == v) return;
    m_config.diagnosticsVisible = v;
    emit layoutChanged();
}

void PlaybackController::setDateNavVisible(bool v) {
    if (m_dateNavVisible == v) return;
    m_dateNavVisible = v;
    emit layoutChanged();
}

QObject* PlaybackController::timeline() const { return m_timeline; }
QObject* PlaybackController::dateNav() const { return m_dateNav; }
QObject* PlaybackController::bookmarks() const { return m_bookmarks; }
QObject* PlaybackController::diagnostics() const { return m_diagnostics; }
QObject* PlaybackController::health() const { return m_healthModel; }

QString PlaybackController::activeAudioName() const {
    const int idx = audioStreamIndex();
    if (idx < 0 || idx >= m_config.streams.size()) {
        return QStringLiteral("未选择音源");
    }
    return m_config.streams[idx].displayName;
}

QVariantList PlaybackController::audioSourceList() const {
    QVariantList list;
    const int curAudio = audioStreamIndex();
    const auto diags = m_scheduler ? m_scheduler->allDiagnostics() : QVector<StreamDiagnostics>{};
    for (int i = 0; i < m_config.streams.size(); ++i) {
        const auto& s = m_config.streams[i];
        QVariantMap m;
        m.insert(QStringLiteral("streamIndex"), s.streamIndex);
        m.insert(QStringLiteral("displayName"), s.displayName);
        m.insert(QStringLiteral("selected"), s.streamIndex == curAudio);
        bool hasVideo = false;
        bool hasAudio = false;
        if (i < diags.size()) {
            hasVideo = (diags[i].state != DecodeState::NoVideo && diags[i].state != DecodeState::Indexing &&
                        diags[i].state != DecodeState::Corrupt && diags[i].state != DecodeState::Disconnected);
            hasAudio = diags[i].hasAudioTrack;
        }
        m.insert(QStringLiteral("hasVideo"), hasVideo);
        m.insert(QStringLiteral("hasAudio"), hasAudio);
        list.append(m);
    }
    return list;
}

QVariantList PlaybackController::cameraConfigs() const {
    QVariantList out;
    for (const auto& c : m_config.cameras) {
        QVariantMap m;
        m.insert(QStringLiteral("cameraId"), c.cameraId);
        m.insert(QStringLiteral("displayName"), c.displayName);
        m.insert(QStringLiteral("rootPath"), c.rootPath);
        m.insert(QStringLiteral("isDual"), c.isDual);
        out.append(m);
    }
    return out;
}

QVariantList PlaybackController::streamConfigs() const {
    QVariantList out;
    for (const auto& s : m_config.streams) {
        QVariantMap m;
        m.insert(QStringLiteral("streamIndex"), s.streamIndex);
        m.insert(QStringLiteral("streamKey"), s.streamKey);
        m.insert(QStringLiteral("cameraId"), s.cameraId);
        m.insert(QStringLiteral("channel"), s.channel);
        m.insert(QStringLiteral("rootPath"), s.rootPath);
        m.insert(QStringLiteral("displayName"), s.displayName);
        m.insert(QStringLiteral("offsetMs"), static_cast<qint64>(m_scheduler->streamOffsetMs(s.streamIndex)));
        out.append(m);
    }
    return out;
}

void PlaybackController::togglePlay() { m_scheduler->clock()->togglePlay(); }
void PlaybackController::play() { m_scheduler->clock()->play(); }
void PlaybackController::pause() { m_scheduler->clock()->pause(); }
void PlaybackController::setPlaybackRate(double rate) { m_scheduler->clock()->setRate(rate); }

void PlaybackController::seekToMs(qint64 wallTimeMs) {
    m_scheduler->seekTo(wallTimeMs);
    m_timeline->ensureTimeVisible(wallTimeMs);
}

qint64 PlaybackController::parseJump(const QString& input) const {
    const auto text = input.trimmed();
    const auto tz = m_config.timezoneOffsetSec;
    if (const auto full = TimeUtils::parseDateTimeString(text, tz)) return *full;

    static const QRegularExpression relative(QStringLiteral(R"(^([+-])(\d{1,8})([smh])$)"),
                                             QRegularExpression::CaseInsensitiveOption);
    if (const auto match = relative.match(text); match.hasMatch()) {
        const auto unit = match.captured(3).toLower();
        const qint64 seconds = match.captured(2).toLongLong() * (unit == u"h" ? 3600LL : unit == u"m" ? 60LL : 1LL);
        const auto result = masterTimeMs() + (match.captured(1) == u"-" ? -seconds : seconds) * 1000;
        return result >= 0 ? result : -1;
    }

    static const QRegularExpression clock(QStringLiteral(
        R"(^(?:(昨天)|(\d{1,2})\.(\d{1,2}))?\s*(?:(\d{2})(\d{2})|(\d{1,2}):(\d{2})(?::(\d{2}))?)$)"));
    const auto match = clock.match(text);
    if (!match.hasMatch()) return -1;
    auto date = QDate::fromString(masterDateText(), Qt::ISODate);
    if (!match.captured(1).isEmpty()) {
        date = QDate::fromString(m_dateNav->yesterdayDateStr(), Qt::ISODate);
    } else if (!match.captured(2).isEmpty()) {
        date = QDate(date.year(), match.captured(2).toInt(), match.captured(3).toInt());
    }
    const bool compact = !match.captured(4).isEmpty();
    const QTime time(match.captured(compact ? 4 : 6).toInt(),
                     match.captured(compact ? 5 : 7).toInt(),
                     match.captured(8).toInt());
    if (!date.isValid() || !time.isValid()) return -1;
    return TimeUtils::civilToEpochMs(date.year(), date.month(), date.day(),
        time.hour(), time.minute(), time.second(), 0, tz);
}

bool PlaybackController::jumpToDateTimeString(const QString& text) {
    const auto target = parseJump(text);
    if (target < 0) {
        emit toastMessage(QStringLiteral("时间格式无效"), true);
        return false;
    }
    seekToMs(target);
    return true;
}

void PlaybackController::jumpToDateKeepTime(const QString& yyyyMmDd, bool exactOnly) {
    const auto dayStartOpt = TimeUtils::parseDateTimeString(yyyyMmDd + QStringLiteral(" 00:00:00"), m_config.timezoneOffsetSec);
    if (!dayStartOpt.has_value()) return;
    const int64_t dayStartMs = *dayStartOpt;
    const int64_t curMs = masterTimeMs();

    // 点击同一天时不做无意义的重新定位与解码
    if (curMs > 0 && TimeUtils::formatDateOnlyMs(curMs, m_config.timezoneOffsetSec) == yyyyMmDd) {
        return;
    }

    int64_t timeOfDayMs = 12LL * 3600 * 1000LL;
    if (curMs > 0) {
        int y = 0, m = 0, d = 0, h = 0, mi = 0, s = 0, ms = 0;
        TimeUtils::epochMsToCivil(curMs, y, m, d, h, mi, s, ms, m_config.timezoneOffsetSec);
        timeOfDayMs = ((static_cast<int64_t>(h) * 3600LL + static_cast<int64_t>(mi) * 60LL + static_cast<int64_t>(s)) * 1000LL) + ms;
    }

    int64_t targetMs = dayStartMs + timeOfDayMs;
    if (!exactOnly && !m_scheduler->hasAnyStreamAt(targetMs)) {
        if (const auto nearest = m_scheduler->findNearestTimeInDay(dayStartMs, targetMs)) {
            targetMs = *nearest;
        }
    }

    m_timeline->fitFullDay(targetMs);
    m_dateNav->updateMasterTime(targetMs);
    m_scheduler->seekTo(targetMs);
}

void PlaybackController::jumpToDateAtFraction(const QString& yyyyMmDd, double dayFraction) {
    const auto dayStartOpt = TimeUtils::parseDateTimeString(yyyyMmDd + QStringLiteral(" 00:00:00"), m_config.timezoneOffsetSec);
    if (!dayStartOpt.has_value()) return;
    const double f = std::clamp(dayFraction, 0.0, 0.99999);
    const int64_t offsetMs = std::clamp(static_cast<int64_t>(std::llround(f * 86400000.0)), 0LL, 86399000LL);
    const int64_t targetMs = *dayStartOpt + offsetMs;

    m_timeline->fitFullDay(targetMs);
    m_dateNav->updateMasterTime(targetMs);
    m_scheduler->seekTo(targetMs);
}

void PlaybackController::jumpToToday(bool exactOnly) {
    if (m_dateNav) {
        jumpToDateKeepTime(m_dateNav->todayDateStr(), exactOnly);
    }
}

void PlaybackController::jumpToYesterday(bool exactOnly) {
    if (m_dateNav) {
        jumpToDateKeepTime(m_dateNav->yesterdayDateStr(), exactOnly);
    }
}

QVariantMap PlaybackController::queryStreamNoVideoNeighbors(int streamIndex) const {
    QVariantMap m;
    const auto info = m_scheduler ? m_scheduler->neighborSegmentsAt(streamIndex, masterTimeMs()) : NeighborSegmentInfo{};
    m.insert(QStringLiteral("hasPrev"), info.hasPrev);
    m.insert(QStringLiteral("prevEndText"),
             info.hasPrev ? TimeUtils::formatEpochMs(info.prevEndWallMs, m_config.timezoneOffsetSec)
                          : QStringLiteral("没有更早录像"));
    m.insert(QStringLiteral("prevJumpMs"), static_cast<qint64>(info.prevJumpWallMs));
    m.insert(QStringLiteral("hasNext"), info.hasNext);
    m.insert(QStringLiteral("nextStartText"),
             info.hasNext ? TimeUtils::formatEpochMs(info.nextStartWallMs, m_config.timezoneOffsetSec)
                          : QStringLiteral("没有更晚录像"));
    m.insert(QStringLiteral("nextJumpMs"), static_cast<qint64>(info.nextJumpWallMs));
    return m;
}

void PlaybackController::jumpStreamPrevSegment(int streamIndex) {
    if (!m_scheduler) return;
    const auto info = m_scheduler->neighborSegmentsAt(streamIndex, masterTimeMs());
    if (info.hasPrev && info.prevJumpWallMs > 0) {
        seekToMs(info.prevJumpWallMs);
    }
}

void PlaybackController::jumpStreamNextSegment(int streamIndex) {
    if (!m_scheduler) return;
    const auto info = m_scheduler->neighborSegmentsAt(streamIndex, masterTimeMs());
    if (info.hasNext && info.nextJumpWallMs > 0) {
        seekToMs(info.nextJumpWallMs);
    }
}

void PlaybackController::stepSeconds(int deltaSec) {
    const qint64 target = masterTimeMs() + static_cast<qint64>(deltaSec) * 1000LL;
    seekToMs(target);
}

void PlaybackController::nextSegment() {
    m_scheduler->jumpToNextSegment();
    m_timeline->ensureTimeVisible(masterTimeMs());
}

void PlaybackController::prevSegment() {
    m_scheduler->jumpToPrevSegment();
    m_timeline->ensureTimeVisible(masterTimeMs());
}

void PlaybackController::handleWheelScrub(bool towardPast, int steps) {
    m_scheduler->clock()->handleWheelScrub(
        towardPast, steps, m_scheduler->globalMinTimeMs(), m_scheduler->globalMaxTimeMs());
    m_timeline->ensureTimeVisible(masterTimeMs());
}

void PlaybackController::beginTimelineScrub() { m_scheduler->clock()->beginTimelineScrub(); }
void PlaybackController::updateTimelineScrub(qint64 targetMs) { m_scheduler->clock()->updateTimelineScrub(targetMs); }
void PlaybackController::endTimelineScrub() { m_scheduler->clock()->endTimelineScrub(); }

void PlaybackController::toggleAudioStream(int streamIndex) {
    if (m_scheduler->selectedAudioStream() == streamIndex) {
        selectManualAudioStream(-1);
    } else {
        selectManualAudioStream(streamIndex);
    }
}

void PlaybackController::selectManualAudioStream(int streamIndex) {
    if (m_config.lastFocusedStream == -1) {
        m_config.manualGridAudioStream = streamIndex;
    } else if (streamIndex == 0 || streamIndex == 1) {
        m_config.lastDualAudioStream = streamIndex;
    }
    m_scheduler->selectAudioStream(streamIndex);
    emit audioChanged();
}

void PlaybackController::setAudioVolume(double vol) {
    const double clamped = std::clamp(vol, 0.0, 1.0);
    if (std::abs(m_config.audioVolume - clamped) < 0.002) return;
    m_config.audioVolume = clamped;
    m_scheduler->audioPlayer()->setVolume(clamped);
    emit audioChanged();
}

void PlaybackController::toggleAudioMute() {
    m_config.audioMuted = !m_config.audioMuted;
    m_scheduler->audioPlayer()->setMuted(m_config.audioMuted);
    emit audioChanged();
}

void PlaybackController::resetAudioVolume100() {
    m_config.audioVolume = 1.0;
    m_scheduler->audioPlayer()->setVolume(1.0);
    emit audioChanged();
    emit toastMessage(m_config.audioMuted ? QStringLiteral("音量已预置为 100%（当前保持静音）")
                                          : QStringLiteral("音量已恢复 100%"), false);
}

void PlaybackController::setAudioFollowsFocus(bool enabled) {
    if (m_config.audioFollowsFocus == enabled) return;
    m_config.audioFollowsFocus = enabled;
    applyAudioRoutingForFocus(m_config.lastFocusedStream);
    saveState();
    emit settingsChanged();
}

void PlaybackController::onAudioMeterTick() {
    if (!m_scheduler) return;
    const int curIdx = m_scheduler->selectedAudioStream();
    bool hasTrack = false;
    if (curIdx >= 0) {
        const auto diags = m_scheduler->allDiagnostics();
        if (curIdx < diags.size()) {
            hasTrack = diags[curIdx].hasAudioTrack &&
                       diags[curIdx].state != DecodeState::NoVideo &&
                       diags[curIdx].state != DecodeState::Indexing &&
                       diags[curIdx].state != DecodeState::Buffering;
        }
    }

    const bool allowActive = m_scheduler->clock()->isPlaying() &&
                             !m_scheduler->clock()->isScrubbing() &&
                             m_scheduler->clock()->playbackRate() <= 2.2 &&
                             hasTrack;
    const AudioMeterState ms = m_scheduler->audioPlayer()->sampleMeter(allowActive);

    const bool changed =
        (m_activeAudioHasTrack != hasTrack) ||
        (m_meterChannels != ms.channels) ||
        (m_audioSignalActive != ms.signalActive) ||
        (std::abs(m_rmsNormL - ms.rmsNormL) > 0.005) ||
        (std::abs(m_rmsNormR - ms.rmsNormR) > 0.005) ||
        (std::abs(m_peakHoldL - ms.peakHoldNormL) > 0.005) ||
        (std::abs(m_peakHoldR - ms.peakHoldNormR) > 0.005);

    if (!changed) return;

    m_activeAudioHasTrack = hasTrack;
    m_meterChannels = ms.channels;
    m_rmsNormL = ms.rmsNormL;
    m_rmsNormR = ms.rmsNormR;
    m_peakHoldL = ms.peakHoldNormL;
    m_peakHoldR = ms.peakHoldNormR;
    m_audioSignalActive = ms.signalActive;
    const double maxPeakDb = std::max(ms.peakDbL, ms.peakDbR);
    m_peakDbText = (maxPeakDb <= -59.5) ? QStringLiteral("-∞ dB")
                                        : QStringLiteral("%1 dB").arg(maxPeakDb, 0, 'f', 1);
    emit audioMeterChanged();
}

void PlaybackController::reportScanIssue(const QString& issue) {
    if (m_scanIssues.contains(issue)) return;
    m_scanIssues.append(issue);
    emit scanChanged();
}

void PlaybackController::rescanDirectories() {
    m_scanIssues.clear();
    m_scanProgress = {true, false, 0, 0, QStringLiteral("正在扫描录像目录…")};
    emit scanChanged();
    m_scheduler->setIndexingActive(true);
    m_scanner->startScan(m_config, true);
}

void PlaybackController::recheckStreamAtCurrentTime(int streamIndex) {
    if (!m_scanner) return;
    emit toastMessage(QStringLiteral("正在局部复查该通道最新录像..."), false);
    m_scanner->startCameraRescan(m_config, streamIndex);
}

void PlaybackController::jumpToHealthLastVideo(qint64 jumpMs) {
    if (jumpMs > 0) {
        seekToMs(jumpMs);
    }
}

void PlaybackController::updateHealthSettings(bool autoCheck, int intervalMin,
                                              bool notifyOffline, bool notifyRecovered) {
    m_config.healthAutoCheck = autoCheck;
    m_config.healthCheckIntervalMin = std::clamp(intervalMin, 1, 120);
    m_config.healthNotifyOffline = notifyOffline;
    m_config.healthNotifyRecovered = notifyRecovered;
    if (m_healthMonitor) {
        m_healthMonitor->configure(m_config);
    }
    saveState();
    emit settingsChanged();
    emit toastMessage(QStringLiteral("健康监测设置已保存"), false);
}

void PlaybackController::updateStreamNames(const QVariantList& names) {
    for (int i = 0; i < std::min<int>(names.size(), m_config.streams.size()); ++i) {
        const QString n = names.at(i).toString().trimmed();
        if (!n.isEmpty()) {
            m_config.streams[i].displayName = n;
        }
    }
    // 同步更新物理摄像头自定义名称（供右侧「录像健康」卡片展示）
    for (auto& cam : m_config.cameras) {
        for (const auto& s : m_config.streams) {
            if (s.cameraId != cam.cameraId) continue;
            if (!cam.isDual) {
                cam.displayName = s.displayName;
                break;
            }
            if (s.channel == QLatin1String("00")) {
                const int sep = s.displayName.indexOf(QStringLiteral("·"));
                const QString prefix = (sep > 0) ? s.displayName.left(sep).trimmed() : s.displayName.trimmed();
                if (!prefix.isEmpty()) {
                    cam.displayName = prefix;
                }
                break;
            }
        }
    }
    if (m_db) {
        m_db->syncStreamsConfig(m_config.cameras, m_config.streams);
    }
    if (m_scheduler) {
        m_scheduler->updateStreamDisplayNames(m_config.streams);
    }
    if (m_healthMonitor) {
        m_healthMonitor->configure(m_config);
        m_healthMonitor->triggerCheckNow();
    }
    saveState();
    emit settingsChanged();
    emit audioChanged();
    emit toastMessage(QStringLiteral("监控机位名称已更新"), false);
}

QVariantList PlaybackController::runAutoOcrCalibration(bool apply) {
    const auto res = m_scheduler->verifyCurrentFramesOcr(apply);
    QVariantList out;
    int okCount = 0;
    for (const auto& r : res) {
        QVariantMap m;
        m.insert(QStringLiteral("streamIndex"), r.streamIndex);
        m.insert(QStringLiteral("ok"), r.ok);
        m.insert(QStringLiteral("ocrTime"), r.ocr.formattedText);
        m.insert(QStringLiteral("confidence"), r.ocr.avgConfidence);
        m.insert(QStringLiteral("suggestedOffsetMs"), static_cast<qint64>(r.suggestedOffsetMs));
        m.insert(QStringLiteral("appliedOffsetMs"), static_cast<qint64>(r.currentOffsetMs));
        if (r.ok) ++okCount;
        out.append(m);
    }
    if (apply) {
        for (int i = 0; i < m_config.streams.size(); ++i) {
            m_config.streams[i].defaultOffsetMs = m_scheduler->streamOffsetMs(i);
        }
        saveState();
        emit settingsChanged();
        emit toastMessage(QStringLiteral("OCR 自动校准完成：成功识别 %1/%2 路").arg(okCount).arg(m_config.streams.size()), okCount == 0);
    }
    return out;
}

void PlaybackController::setStreamOffsetMs(int streamIndex, qint64 offsetMs) {
    m_scheduler->setStreamOffsetMs(streamIndex, offsetMs);
    if (streamIndex >= 0 && streamIndex < m_config.streams.size()) {
        m_config.streams[streamIndex].defaultOffsetMs = offsetMs;
    }
    saveState();
}

QString PlaybackController::captureFrameScreenshot(int streamIndex) {
    int targetIdx = streamIndex;
    if (targetIdx < 0 || targetIdx >= m_scheduler->streamCount()) {
        targetIdx = (m_config.lastFocusedStream >= 0 && m_config.lastFocusedStream < m_scheduler->streamCount())
            ? m_config.lastFocusedStream
            : 0;
    }

    auto frame = m_scheduler->latestFrame(targetIdx);
    if (!frame || !frame->nv12Data) {
        for (int i = 0; i < m_scheduler->streamCount(); ++i) {
            if (auto f = m_scheduler->latestFrame(i); f && f->nv12Data) {
                frame = f;
                targetIdx = i;
                break;
            }
        }
    }
    if (!frame || !frame->nv12Data) return QString();

    QImage img = nv12FrameToQImage(*frame);
    if (img.isNull()) return QString();

    QDir().mkpath(m_config.screenshotDir);
    const QString fileName = QStringLiteral("mark_cam%1_%2.png")
                                 .arg(targetIdx)
                                 .arg(TimeUtils::nowEpochMs());
    const QString outPath = QDir(m_config.screenshotDir).filePath(fileName);
    if (img.scaledToWidth(std::min(img.width(), 960), Qt::SmoothTransformation).save(outPath, "PNG")) {
        return outPath;
    }
    return QString();
}

void PlaybackController::addBookmarkWithCapture(const QString& title, const QString& note,
                                                const QString& color, int streamIndex) {
    const qint64 ts = masterTimeMs();
    const QString shotPath = captureFrameScreenshot(streamIndex);
    m_bookmarks->addBookmark(ts, title, note, color, streamIndex, shotPath);
    emit toastMessage(QStringLiteral("已添加标记: %1").arg(title), false);
}

void PlaybackController::setDeveloperDiagnostics(bool enabled) {
    if (m_config.developerDiagnostics == enabled) return;
    m_config.developerDiagnostics = enabled;
    saveState();
    emit settingsChanged();
}

void PlaybackController::updateCacheAndDecodeSettings(const QString& cacheDir,
                                                      int cacheMaxGb,
                                                      bool preferHw) {
    m_config.cacheDir = cacheDir.trimmed().isEmpty() ? m_config.cacheDir : cacheDir.trimmed();
    m_config.cacheMaxGb = std::clamp(cacheMaxGb, 1, 500);
    m_config.preferHwDecode = preferHw;
    m_cache->configure(m_config.cacheDir, m_config.cacheMaxGb, m_config.memCacheMaxMb, m_config.maxConcurrentNasReads);
    if (m_scheduler) {
        m_scheduler->setPreferHwDecode(preferHw);
    }
    saveState();
    emit settingsChanged();
    emit toastMessage(QStringLiteral("缓存与解码设置已保存"), false);
}

namespace {
QString cameraDirectoryIssue(const QString& path) {
    if (path.isEmpty() || !QDir::isAbsolutePath(path)) {
        return QStringLiteral("请选择录像文件夹，或输入完整路径。");
    }
    const QFileInfo directory(path);
    if (!directory.isDir() || !directory.isReadable()) {
        return QStringLiteral("无法读取录像目录。请先在系统文件管理器打开它，确认 NAS 已连接、路径正确且有读取权限：%1").arg(path);
    }
    return {};
}

bool sameCameraDirectory(const QString& a, const QString& b) {
    const auto left = QDir::cleanPath(QDir::fromNativeSeparators(a));
    const auto right = QDir::cleanPath(QDir::fromNativeSeparators(b));
#ifdef SYNC_PLATFORM_WINDOWS
    return left.compare(right, Qt::CaseInsensitive) == 0;
#else
    return left == right;
#endif
}

QString saveCameraConfigIssue(const AppConfig& config) {
    if (config.saveToJson(config.configFilePath())) return {};
    return QStringLiteral("无法保存配置。请检查应用数据目录可写且有剩余空间：%1").arg(config.dataDir);
}
}

QString PlaybackController::updateCameraDirectories(const QVariantList& paths, bool rescanNow) {
    auto updated = m_config;
    for (int i = 0; i < std::min<int>(paths.size(), updated.cameras.size()); ++i) {
        const auto path = QDir::fromNativeSeparators(paths.at(i).toString().trimmed());
        if (path == m_config.cameras[i].rootPath) continue;
        const auto issue = cameraDirectoryIssue(path);
        if (!issue.isEmpty()) return updated.cameras[i].displayName + QStringLiteral("：") + issue;
        updated.cameras[i].rootPath = path;
    }
    for (int i = 0; i < updated.cameras.size(); ++i) {
        for (int j = 0; j < i; ++j) {
            if (sameCameraDirectory(updated.cameras[i].rootPath, updated.cameras[j].rootPath)
                && !sameCameraDirectory(m_config.cameras[i].rootPath, m_config.cameras[j].rootPath)) {
                return QStringLiteral("两个摄像机使用了同一个目录。双摄请添加一次并勾选「双摄」。");
            }
        }
    }
    const auto saveIssue = saveCameraConfigIssue(updated);
    if (!saveIssue.isEmpty()) return saveIssue;

    if (rescanNow) m_scanner->stop();
    m_config = std::move(updated);
    m_config.rebuildStreamsFromCameras();
    if (m_healthMonitor) m_healthMonitor->configure(m_config);
    emit settingsChanged();
    if (rescanNow) {
        m_scheduler->initialize(m_config);
        applyAudioRoutingForFocus(m_config.lastFocusedStream);
        rescanDirectories();
        if (m_healthMonitor) m_healthMonitor->triggerCheckNow();
    }
    return {};
}

bool PlaybackController::detectDualCameraDir(const QString& rootPath) const {
    const QString clean = QDir::fromNativeSeparators(rootPath.trimmed());
    if (clean.isEmpty() || !QDir(clean).exists()) return false;
    QDirIterator it(clean, {QStringLiteral("10_*.mp4")}, QDir::Files, QDirIterator::Subdirectories);
    return it.hasNext();
}

QString PlaybackController::addCamera(const QString& displayName, const QString& rootPath, bool isDual) {
    const QString cleanPath = QDir::fromNativeSeparators(rootPath.trimmed());
    const auto issue = cameraDirectoryIssue(cleanPath);
    if (!issue.isEmpty()) return issue;
    for (const auto& camera : m_config.cameras) {
        if (sameCameraDirectory(camera.rootPath, cleanPath)) {
            return QStringLiteral("此录像目录已添加。请在「设置 → 机位」中查看或修改。");
        }
    }
    const QFileInfo fi(cleanPath);
    QString baseId = fi.fileName().trimmed();
    if (baseId.isEmpty()) {
        baseId = QStringLiteral("CAM");
    }
    QString camId = baseId;
    int suffix = 2;
    auto hasId = [&](const QString& id) {
        for (const auto& c : m_config.cameras) {
            if (c.cameraId == id) return true;
        }
        return false;
    };
    while (hasId(camId)) {
        camId = QStringLiteral("%1_%2").arg(baseId).arg(suffix++);
    }

    CameraConfig cam;
    cam.cameraId = camId;
    cam.displayName = displayName.trimmed().isEmpty() ? camId : displayName.trimmed();
    cam.rootPath = cleanPath;
    cam.isDual = isDual;
    cam.sortOrder = m_config.cameras.size();
    cam.recursive = true;
    cam.maxDepth = isDual ? 3 : 2;
    auto updated = m_config;
    updated.cameras.append(cam);
    const auto saveIssue = saveCameraConfigIssue(updated);
    if (!saveIssue.isEmpty()) return saveIssue;
    m_config = std::move(updated);
    reloadCameraConfiguration();
    emit toastMessage(QStringLiteral("已添加摄像头「%1」，开始扫描录像").arg(cam.displayName), false);
    return {};
}

bool PlaybackController::removeCamera(const QString& cameraId) {
    bool found = false;
    QString removedName;
    for (int i = 0; i < m_config.cameras.size(); ++i) {
        if (m_config.cameras[i].cameraId == cameraId) {
            removedName = m_config.cameras[i].displayName;
            m_config.cameras.removeAt(i);
            found = true;
            break;
        }
    }
    if (!found) return false;

    for (int i = 0; i < m_config.cameras.size(); ++i) {
        m_config.cameras[i].sortOrder = i;
    }
    reloadCameraConfiguration();
    emit toastMessage(QStringLiteral("已移除摄像头「%1」").arg(removedName), false);
    return true;
}

void PlaybackController::reloadCameraConfiguration() {
    m_scanner->stop();
    const auto previousStreams = m_config.streams;
    const auto audioIndex = m_config.manualGridAudioStream;
    const QString audioKey = audioIndex >= 0 && audioIndex < previousStreams.size()
        ? previousStreams[audioIndex].streamKey : QString();
    m_config.rebuildStreamsFromCameras();
    m_config.lastFocusedStream = -1;
    m_config.manualGridAudioStream = 0;
    for (const auto& stream : m_config.streams) {
        if (stream.streamKey == audioKey) {
            m_config.manualGridAudioStream = stream.streamIndex;
            break;
        }
    }
    m_lastAutoRescanMs.clear();
    m_initialSeekDone = false;
    m_scheduler->clock()->pause();
    saveState();

    m_db->syncStreamsConfig(m_config.cameras, m_config.streams);
    m_bookmarks->reload();
    m_scheduler->initialize(m_config);
    applyAudioRoutingForFocus(-1);
    for (const auto& item : m_videoItems) {
        if (item) item->updateFrame(nullptr, DecodeState::Idle, m_config.timezoneOffsetSec);
    }
    m_healthMonitor->configure(m_config);
    m_healthMonitor->triggerCheckNow();
    m_timeline->refreshFromScheduler();
    m_dateNav->refreshFromScheduler();
    m_diagnostics->refresh();

    emit settingsChanged();
    emit layoutChanged();
    emit audioChanged();
    rescanDirectories();
}

QString PlaybackController::normalizedShortcut(const QString& sequence) const {
    return QKeySequence::fromString(sequence, QKeySequence::PortableText).toString(QKeySequence::PortableText);
}

void PlaybackController::updateShortcuts(const QString& togglePlay, const QString& addBookmark,
                                         const QString& stepBack, const QString& stepForward,
                                         const QString& exitFocus) {
    if (!togglePlay.isEmpty()) m_config.shortcuts.togglePlay = togglePlay;
    if (!addBookmark.isEmpty()) m_config.shortcuts.addBookmark = addBookmark;
    if (!stepBack.isEmpty()) m_config.shortcuts.stepBackward = stepBack;
    if (!stepForward.isEmpty()) m_config.shortcuts.stepForward = stepForward;
    if (!exitFocus.isEmpty()) m_config.shortcuts.exitFocus = exitFocus;
    saveState();
    emit settingsChanged();
}

void PlaybackController::setHighlightedCameraId(const QString& camId) {
    if (m_highlightedCameraId == camId) return;
    m_highlightedCameraId = camId;
    emit highlightChanged();
}

QVariantList PlaybackController::presetCities() const {
    struct CityItem {
        const char* name;
        double lat;
        double lon;
        int tzHours;
    };
    static const CityItem kCities[] = {
        {"北京", 39.9042, 116.4074, 8},
        {"上海", 31.2304, 121.4737, 8},
        {"广州", 23.1291, 113.2644, 8},
        {"深圳", 22.5431, 114.0579, 8},
        {"杭州", 30.2741, 120.1551, 8},
        {"成都", 30.5728, 104.0668, 8},
        {"重庆", 29.5630, 106.5516, 8},
        {"武汉", 30.5928, 114.3055, 8},
        {"西安", 34.3416, 108.9398, 8},
        {"南京", 32.0603, 118.7969, 8},
        {"苏州", 31.2989, 120.5853, 8},
        {"厦门", 24.4798, 118.0894, 8},
        {"沈阳", 41.8057, 123.4315, 8},
        {"哈尔滨", 45.8038, 126.5350, 8},
        {"乌鲁木齐", 43.8256, 87.6168, 8},
        {"香港", 22.3193, 114.1694, 8},
        {"台北", 25.0330, 121.5654, 8},
        {"东京", 35.6762, 139.6503, 9},
        {"新加坡", 1.3521, 103.8198, 8},
        {"伦敦", 51.5074, -0.1278, 0},
        {"纽约", 40.7128, -74.0060, -5},
        {"洛杉矶", 34.0522, -118.2437, -8},
        {"悉尼", -33.8688, 151.2093, 10},
    };
    QVariantList out;
    for (const auto& c : kCities) {
        QVariantMap m;
        m.insert(QStringLiteral("name"), QString::fromUtf8(c.name));
        m.insert(QStringLiteral("lat"), c.lat);
        m.insert(QStringLiteral("lon"), c.lon);
        m.insert(QStringLiteral("tzHours"), c.tzHours);
        out.append(m);
    }
    return out;
}

void PlaybackController::updateLocationSettings(bool configured, const QString& city,
                                                  double lat, double lon, int tzHours) {
    m_config.locationConfigured = configured;
    m_config.locationCity = city.trimmed().isEmpty()
        ? (configured ? QStringLiteral("自定义坐标") : QStringLiteral("未配置（固定 06:00–18:00）"))
        : city.trimmed();
    m_config.latitude = std::clamp(lat, -89.9, 89.9);
    m_config.longitude = std::clamp(lon, -180.0, 180.0);
    m_config.timezoneOffsetSec = std::clamp(tzHours, -12, 14) * 3600;

    SunTimesService::instance().configure(m_config.locationConfigured, m_config.latitude,
                                          m_config.longitude, m_config.timezoneOffsetSec);

    if (m_timeline) {
        m_timeline->setTimezoneOffsetSec(m_config.timezoneOffsetSec);
        m_timeline->refreshFromScheduler();
    }
    if (m_dateNav) {
        m_dateNav->setTimezoneOffsetSec(m_config.timezoneOffsetSec);
        m_dateNav->refreshFromScheduler();
    }
    if (m_diagnostics) {
        m_diagnostics->setTimezoneOffsetSec(m_config.timezoneOffsetSec);
        m_diagnostics->refresh();
    }
    if (m_bookmarks) {
        m_bookmarks->setTimezoneOffsetSec(m_config.timezoneOffsetSec);
    }
    if (m_healthModel) {
        m_healthModel->setTimezoneOffsetSec(m_config.timezoneOffsetSec);
    }

    saveState();
    emit settingsChanged();
    emit masterTimeChanged();
    if (configured) {
        emit toastMessage(QStringLiteral("所在地已更新为「%1」，时间轴将按真实日出日落区分昼夜").arg(m_config.locationCity), false);
    } else {
        emit toastMessage(QStringLiteral("已切换为固定 06:00–18:00 昼夜划分模式"), false);
    }
}

void PlaybackController::updateTimelineColors(const QString& dayColor, const QString& nightColor) {
    auto normalizeHex = [](const QString& in, const QString& fallback) -> QString {
        QString s = in.trimmed().toUpper();
        if (!s.startsWith(QLatin1Char('#'))) {
            s.prepend(QLatin1Char('#'));
        }
        if (s.size() != 7) return fallback;
        for (int i = 1; i < 7; ++i) {
            const QChar c = s.at(i);
            const bool isHex = (c >= QLatin1Char('0') && c <= QLatin1Char('9')) ||
                               (c >= QLatin1Char('A') && c <= QLatin1Char('F'));
            if (!isHex) return fallback;
        }
        return s;
    };

    m_config.timelineDayColor = normalizeHex(dayColor, QStringLiteral("#3485EF"));
    m_config.timelineNightColor = normalizeHex(nightColor, QStringLiteral("#7975C9"));
    saveState();
    emit settingsChanged();
}

void PlaybackController::clearAllCaches() {
    if (m_cache) {
        m_cache->clearAllCaches();
        m_diagnostics->refresh();
        emit toastMessage(QStringLiteral("本地内存与 SSD 缓存已清空"), false);
    }
}

void PlaybackController::saveState() {
    if (m_scheduler) {
        const qint64 t = m_scheduler->clock()->currentTimeMs();
        if (t > 0) m_config.lastMasterTimeMs = t;
        m_config.lastPlaybackRate = m_scheduler->clock()->playbackRate();
    }
    m_config.saveToJson(m_config.configFilePath());
}

} // namespace mcs
