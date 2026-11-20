#include "sync/SyncScheduler.h"
#include "cache/NasBlockCache.h"
#include "core/Logger.h"
#include "core/TimeUtils.h"
#include "index/IndexDatabase.h"

#include <QSet>
#include <algorithm>
#include <cmath>
#include <limits>

namespace mcs {

SyncScheduler::SyncScheduler(IndexDatabase* db, NasBlockCache* cache, QObject* parent)
    : QObject(parent), m_db(db), m_cache(cache) {
    m_tickTimer = new QTimer(this);
    m_tickTimer->setTimerType(Qt::PreciseTimer);
    m_tickTimer->setInterval(16); // ~60 Hz 主时钟轮询与帧分发
    connect(m_tickTimer, &QTimer::timeout, this, &SyncScheduler::onTick);

    m_diagTimer = new QTimer(this);
    m_diagTimer->setInterval(250); // 4 Hz 刷新诊断数据给 QML
    connect(m_diagTimer, &QTimer::timeout, this, &SyncScheduler::diagnosticsUpdated);

    connect(&m_clock, &MasterClock::scrubPreviewRequested, this, &SyncScheduler::onScrubPreview);
    connect(&m_clock, &MasterClock::exactSeekRequested, this, &SyncScheduler::onExactSeek);
}

SyncScheduler::~SyncScheduler() {
    shutdown();
}

void SyncScheduler::initialize(const AppConfig& cfg) {
    shutdown();
    m_cfg = cfg;
    m_streams = cfg.streams;
    const int n = m_streams.size();

    m_streamOffsetsMs.assign(static_cast<size_t>(n), 0);
    m_lastDelivered.assign(static_cast<size_t>(n), nullptr);
    m_lastDeliveredState.assign(static_cast<size_t>(n), DecodeState::Idle);
    m_lastDeliveredWaitMs.assign(static_cast<size_t>(n), 0);

    // 从数据库恢复流级偏移
    const auto dbOffsets = m_db ? m_db->loadStreamOffsets() : QHash<int, int64_t>{};
    for (int i = 0; i < n; ++i) {
        m_streamOffsetsMs[static_cast<size_t>(i)] = dbOffsets.contains(i)
            ? dbOffsets.value(i)
            : m_streams[i].defaultOffsetMs;
        auto dec = std::make_unique<StreamDecoder>(i, m_cache, &m_audio);
        StreamConfig sc = m_streams[i];
        sc.defaultOffsetMs = m_streamOffsetsMs[static_cast<size_t>(i)];
        dec->start(sc, m_cfg.preferHwDecode);
        m_decoders.push_back(std::move(dec));
    }

    reloadIndex();
    m_tickTimer->start();
    m_diagTimer->start();
}

void SyncScheduler::shutdown() {
    if (m_tickTimer) m_tickTimer->stop();
    if (m_diagTimer) m_diagTimer->stop();
    m_audio.setEnabled(false);
    for (auto& d : m_decoders) {
        if (d) d->stop();
    }
    m_decoders.clear();
}

void SyncScheduler::reloadIndex() {
    if (!m_db) return;
    auto loaded = m_db->loadAllSegmentsByStream(static_cast<int>(m_streams.size()));
    int64_t minT = std::numeric_limits<int64_t>::max();
    int64_t maxT = 0;
    for (const auto& vec : loaded) {
        if (!vec.empty()) {
            minT = std::min(minT, vec.front().nominalStartMs);
            maxT = std::max(maxT, vec.back().effectiveEndMs());
        }
    }
    if (minT == std::numeric_limits<int64_t>::max()) {
        minT = 0;
    }

    {
        std::unique_lock lk(m_indexMtx);
        m_segmentsByStream = std::move(loaded);
        m_minTimeMs = minT;
        m_maxTimeMs = maxT;
    }
    emit indexReloaded();
}

void SyncScheduler::findSegmentCovering(int streamIndex,
                                        int64_t wallTimeMs,
                                        std::optional<SegmentRecord>& outCur,
                                        std::optional<SegmentRecord>& outNext) const {
    outCur.reset();
    outNext.reset();
    std::shared_lock lk(m_indexMtx);
    if (streamIndex < 0 || streamIndex >= static_cast<int>(m_segmentsByStream.size())) return;
    const auto& vec = m_segmentsByStream[static_cast<size_t>(streamIndex)];
    if (vec.empty()) return;

    const int64_t streamOff = m_streamOffsetsMs[static_cast<size_t>(streamIndex)];
    // 转换到切片名义时间轴以二分查找：nominal = wallTimeMs - streamOffset
    const int64_t nomTarget = wallTimeMs - streamOff;

    auto it = std::upper_bound(
        vec.begin(), vec.end(), nomTarget,
        [](int64_t t, const SegmentRecord& r) { return t < r.nominalStartMs; });

    auto covers = [nomTarget](const SegmentRecord& r) {
        const int64_t effStart = r.nominalStartMs + r.segmentOffsetMs;
        const int64_t effEnd = r.effectiveEndMs() + r.segmentOffsetMs;
        return nomTarget >= effStart && nomTarget <= effEnd + 250;
    };
    auto setHit = [&](std::vector<SegmentRecord>::const_iterator hit) {
        outCur = *hit;
        const auto nxt = std::next(hit);
        if (nxt != vec.end()) {
            outNext = *nxt;
        }
    };

    if (it != vec.begin()) {
        const auto prevIt = std::prev(it);
        if (covers(*prevIt)) {
            setHit(prevIt);
            return;
        }
        auto scanIt = prevIt;
        for (int back = 0; back < 2 && scanIt != vec.begin(); ++back) {
            --scanIt;
            if (covers(*scanIt)) {
                setHit(scanIt);
                return;
            }
        }
    }
    if (it != vec.end() && covers(*it)) {
        setHit(it);
        return;
    }
    // 当前时刻无录像覆盖，但仍提供下一个切片提示
    if (it != vec.end()) {
        outNext = *it;
    }
}

std::optional<SegmentRecord> SyncScheduler::segmentAt(int streamIndex, int64_t wallTimeMs) const {
    std::optional<SegmentRecord> cur, next;
    findSegmentCovering(streamIndex, wallTimeMs, cur, next);
    return cur;
}

void SyncScheduler::dispatchSeek(int64_t targetMs, bool isScrubbing) {
    m_audio.clearBuffer();
    for (int i = 0; i < static_cast<int>(m_decoders.size()); ++i) {
        std::optional<SegmentRecord> cur, next;
        findSegmentCovering(i, targetMs, cur, next);
        m_decoders[static_cast<size_t>(i)]->requestSeek(cur, next, targetMs, isScrubbing);
    }
}

void SyncScheduler::seekTo(int64_t wallTimeMs) {
    m_clock.seekExact(wallTimeMs);
}

void SyncScheduler::onScrubPreview(int64_t targetMs) {
    dispatchSeek(targetMs, true);
}

void SyncScheduler::onExactSeek(int64_t targetMs, bool resumePlaying) {
    Q_UNUSED(resumePlaying);
    dispatchSeek(targetMs, false);
}

void SyncScheduler::jumpToNextSegment() {
    const int64_t cur = m_clock.currentTimeMs();
    int64_t bestNext = std::numeric_limits<int64_t>::max();
    {
        std::shared_lock lk(m_indexMtx);
        for (size_t i = 0; i < m_segmentsByStream.size(); ++i) {
            const int64_t off = m_streamOffsetsMs[i];
            const int64_t nomCur = cur - off;
            const auto& vec = m_segmentsByStream[i];
            auto it = std::upper_bound(
                vec.begin(), vec.end(), nomCur + 500,
                [](int64_t t, const SegmentRecord& r) { return t < r.nominalStartMs; });
            if (it != vec.end()) {
                bestNext = std::min(bestNext, it->nominalStartMs + off + it->segmentOffsetMs);
            }
        }
    }
    if (bestNext < std::numeric_limits<int64_t>::max()) {
        m_clock.seekExact(bestNext);
    }
}

void SyncScheduler::jumpToPrevSegment() {
    const int64_t cur = m_clock.currentTimeMs();
    int64_t bestPrev = -1;
    {
        std::shared_lock lk(m_indexMtx);
        for (size_t i = 0; i < m_segmentsByStream.size(); ++i) {
            const int64_t off = m_streamOffsetsMs[i];
            const int64_t nomCur = cur - off;
            const auto& vec = m_segmentsByStream[i];
            auto it = std::lower_bound(
                vec.begin(), vec.end(), nomCur - 1000,
                [](const SegmentRecord& r, int64_t t) { return r.nominalStartMs < t; });
            if (it != vec.begin()) {
                --it;
                bestPrev = std::max(bestPrev, it->nominalStartMs + off + it->segmentOffsetMs);
            }
        }
    }
    if (bestPrev > 0) {
        m_clock.seekExact(bestPrev);
    }
}

void SyncScheduler::setPreferHwDecode(bool preferHw) {
    m_cfg.preferHwDecode = preferHw;
    for (auto& d : m_decoders) {
        if (d) {
            d->setPreferHwDecode(preferHw);
        }
    }
}

void SyncScheduler::setIndexingActive(bool active) {
    for (auto& d : m_decoders) {
        if (d) {
            d->setIndexingActive(active);
        }
    }
}

void SyncScheduler::updateStreamDisplayNames(const QVector<StreamConfig>& streams) {
    m_streams = streams;
    m_cfg.streams = streams;
    for (int i = 0; i < static_cast<int>(m_decoders.size()) && i < streams.size(); ++i) {
        if (m_decoders[static_cast<size_t>(i)]) {
            m_decoders[static_cast<size_t>(i)]->setDisplayName(streams[i].displayName);
        }
    }
    emit indexReloaded();
}

void SyncScheduler::selectAudioStream(int streamIndex) {
    if (m_audioStreamIndex == streamIndex) return;
    m_audioStreamIndex = streamIndex;
    m_audio.prepareStreamSwitch();
    const bool allowPlay = m_clock.isPlaying() && !m_clock.isScrubbing() && (m_clock.playbackRate() <= 2.2);
    m_audio.setEnabled(streamIndex >= 0 && allowPlay);
    for (int i = 0; i < static_cast<int>(m_decoders.size()); ++i) {
        m_decoders[static_cast<size_t>(i)]->setAudioSelected(i == streamIndex);
    }
    emit audioStreamChanged(streamIndex);
}

void SyncScheduler::setStreamOffsetMs(int streamIndex, int64_t offsetMs) {
    if (streamIndex < 0 || streamIndex >= static_cast<int>(m_decoders.size())) return;
    {
        std::unique_lock lk(m_indexMtx);
        m_streamOffsetsMs[static_cast<size_t>(streamIndex)] = offsetMs;
    }
    m_decoders[static_cast<size_t>(streamIndex)]->setStreamOffsetMs(offsetMs);
    if (m_db) {
        m_db->updateStreamOffset(streamIndex, offsetMs);
    }
    emit streamOffsetChanged(streamIndex, offsetMs);

    // 刷新该路画面到当前主时钟对应位置
    std::optional<SegmentRecord> cur, next;
    const int64_t nowMs = m_clock.currentTimeMs();
    findSegmentCovering(streamIndex, nowMs, cur, next);
    m_decoders[static_cast<size_t>(streamIndex)]->requestSeek(cur, next, nowMs, m_clock.isScrubbing());
}

int64_t SyncScheduler::streamOffsetMs(int streamIndex) const {
    std::shared_lock lk(m_indexMtx);
    if (streamIndex < 0 || streamIndex >= static_cast<int>(m_streamOffsetsMs.size())) return 0;
    return m_streamOffsetsMs[static_cast<size_t>(streamIndex)];
}

QVector<CalibrationVerifyResult> SyncScheduler::verifyCurrentFramesOcr(bool autoApply) {
    QVector<CalibrationVerifyResult> results;
    for (int i = 0; i < static_cast<int>(m_decoders.size()); ++i) {
        CalibrationVerifyResult r;
        r.streamIndex = i;
        r.currentOffsetMs = streamOffsetMs(i);
        const auto frame = m_decoders[static_cast<size_t>(i)]->latestPresentedFrame();
        if (frame && frame->nv12Data) {
            r.nominalTimeMs = frame->nominalTimeMs;
            r.ocr = TimestampOcr::recognizeFromFrame(*frame, m_cfg.timezoneOffsetSec);
            r.ok = r.ocr.ok;
            if (r.ok) {
                // OSD 水印记录的整秒覆盖区间为 [detectedWallMs, detectedWallMs + 999]。
                // 当 nominalTimeMs 落在 [detectedWallMs - 150, detectedWallMs + 1050] 时属于同一秒量化内，偏移视为 0。
                const int64_t distToInterval =
                    (frame->nominalTimeMs < r.ocr.detectedWallMs - 150)
                    ? (r.ocr.detectedWallMs - frame->nominalTimeMs)
                    : ((frame->nominalTimeMs > r.ocr.detectedWallMs + 1050)
                        ? (r.ocr.detectedWallMs + 1000 - frame->nominalTimeMs)
                        : 0);
                // 按 500ms 步进取整，确保仅在真实时钟偏差时调整
                r.suggestedOffsetMs = (distToInterval == 0)
                    ? 0
                    : static_cast<int64_t>(std::llround(static_cast<double>(distToInterval) / 1000.0) * 1000LL);
                if (autoApply && std::abs(r.suggestedOffsetMs - r.currentOffsetMs) >= 1000 &&
                    std::abs(r.suggestedOffsetMs) <= 3600000LL) {
                    setStreamOffsetMs(i, r.suggestedOffsetMs);
                    if (m_db) {
                        const auto seg = segmentAt(i, m_clock.currentTimeMs());
                        if (seg) {
                            m_db->updateSegmentCalibration(
                                seg->id, 0, r.ocr.avgConfidence, QStringLiteral("auto_ocr"),
                                QStringLiteral("OCR=%1").arg(r.ocr.formattedText));
                        }
                    }
                    r.currentOffsetMs = r.suggestedOffsetMs;
                }
            }
        }
        results.append(r);
    }
    return results;
}

void SyncScheduler::onTick() {
    const int64_t masterMs = m_clock.currentTimeMs();
    const bool playing = m_clock.isPlaying();
    const double rate = m_clock.playbackRate();

    // 音频在 0.5x / 1x / 2x 正常播放时激活输出，4x/8x 及拖拽期间自动临时静音
    m_audio.setPlaybackRate(rate);
    const bool shouldAudioPlay = (m_audioStreamIndex >= 0) && playing && !m_clock.isScrubbing() && (rate <= 2.2);
    if (m_audio.isEnabled() != shouldAudioPlay) {
        m_audio.setEnabled(shouldAudioPlay);
    }

    for (int i = 0; i < static_cast<int>(m_decoders.size()); ++i) {
        auto& dec = m_decoders[static_cast<size_t>(i)];
        std::optional<SegmentRecord> cur, next;
        findSegmentCovering(i, masterMs, cur, next);
        dec->updatePlaybackContext(masterMs, rate, playing && !m_clock.isScrubbing(), cur, next);

        const auto frame = dec->consumeFrameForTime(masterMs, playing && !m_clock.isScrubbing());
        const auto diag = dec->diagnostics(masterMs);
        if (m_frameSink) {
            const bool frameChanged = (frame != m_lastDelivered[static_cast<size_t>(i)]);
            const bool stateChanged = (diag.state != m_lastDeliveredState[static_cast<size_t>(i)]);
            const bool waitTickChanged =
                (diag.state == DecodeState::Buffering || diag.state == DecodeState::Locating) &&
                std::abs(diag.waitDurationMs - m_lastDeliveredWaitMs[static_cast<size_t>(i)]) >= 250;
            if (frameChanged || stateChanged || waitTickChanged) {
                m_lastDelivered[static_cast<size_t>(i)] = frame;
                m_lastDeliveredState[static_cast<size_t>(i)] = diag.state;
                m_lastDeliveredWaitMs[static_cast<size_t>(i)] = diag.waitDurationMs;
                m_frameSink(i, frame, diag.state, diag.waitDurationMs);
            }
        }
    }

    if (playing) {
        emit m_clock.timeChanged(masterMs);
    }
}

int64_t SyncScheduler::globalMinTimeMs() const {
    std::shared_lock lk(m_indexMtx);
    return m_minTimeMs;
}

int64_t SyncScheduler::globalMaxTimeMs() const {
    std::shared_lock lk(m_indexMtx);
    return m_maxTimeMs;
}

QVector<TimeInterval> SyncScheduler::intervalsInRange(int streamIndex, int64_t startMs, int64_t endMs) const {
    QVector<TimeInterval> out;
    std::shared_lock lk(m_indexMtx);
    if (streamIndex < 0 || streamIndex >= static_cast<int>(m_segmentsByStream.size())) return out;
    const auto& vec = m_segmentsByStream[static_cast<size_t>(streamIndex)];
    const int64_t off = m_streamOffsetsMs[static_cast<size_t>(streamIndex)];

    auto it = std::lower_bound(
        vec.begin(), vec.end(), startMs - off - 10800000LL,
        [](const SegmentRecord& r, int64_t t) { return r.nominalStartMs < t; });

    for (; it != vec.end(); ++it) {
        const int64_t s = it->nominalStartMs + off + it->segmentOffsetMs;
        const int64_t e = it->effectiveEndMs() + off + it->segmentOffsetMs;
        if (s > endMs) break;
        if (e < startMs) continue;

        // 相邻间隔 <= 3s 且同为正常状态时自动合并区间，避免绘制上千条碎矩形影响时间轴 60fps
        if (!out.isEmpty() && out.last().status == it->status && s - out.last().endMs <= 3000) {
            out.last().endMs = std::max(out.last().endMs, e);
        } else {
            out.append({std::max(s, startMs), std::min(e, endMs), it->status});
        }
    }
    return out;
}

QVector<int64_t> SyncScheduler::availableDayStartsMs(int tzOffsetSec) const {
    QSet<int64_t> days;
    std::shared_lock lk(m_indexMtx);
    for (size_t i = 0; i < m_segmentsByStream.size(); ++i) {
        const int64_t off = (i < m_streamOffsetsMs.size()) ? m_streamOffsetsMs[i] : 0;
        for (const auto& seg : m_segmentsByStream[i]) {
            days.insert(TimeUtils::dayStartMs(seg.nominalStartMs + off, tzOffsetSec));
        }
    }
    QVector<int64_t> sorted = days.values().toVector();
    std::sort(sorted.begin(), sorted.end());
    return sorted;
}

NeighborSegmentInfo SyncScheduler::neighborSegmentsAt(int streamIndex, int64_t wallTimeMs) const {
    NeighborSegmentInfo info;
    std::shared_lock lk(m_indexMtx);
    if (streamIndex < 0 || streamIndex >= static_cast<int>(m_segmentsByStream.size())) return info;
    const auto& vec = m_segmentsByStream[static_cast<size_t>(streamIndex)];
    if (vec.empty()) return info;

    const int64_t streamOff = m_streamOffsetsMs[static_cast<size_t>(streamIndex)];
    const int64_t nomTarget = wallTimeMs - streamOff;

    auto isValid = [](const SegmentRecord& r) {
        return r.status != SegmentStatus::Corrupt && r.status != SegmentStatus::Missing;
    };

    auto it = std::upper_bound(
        vec.begin(), vec.end(), nomTarget,
        [](int64_t t, const SegmentRecord& r) { return t < r.nominalStartMs; });

    // 查找当前时刻之前最近的有效片段（排除覆盖当前时刻的片段）
    for (auto p = it; p != vec.begin();) {
        --p;
        if (!isValid(*p)) continue;
        const int64_t s = p->nominalStartMs + streamOff + p->segmentOffsetMs;
        const int64_t e = p->effectiveEndMs() + streamOff + p->segmentOffsetMs;
        if (wallTimeMs >= s && wallTimeMs <= e + 250) {
            continue; // 当前正在此切片内
        }
        if (e < wallTimeMs) {
            info.hasPrev = true;
            info.prevEndWallMs = e;
            info.prevJumpWallMs = std::max(s + 300, e - 3000);
            break;
        }
    }

    // 查找当前时刻之后最近的有效片段
    for (auto n = it; n != vec.end(); ++n) {
        if (!isValid(*n)) continue;
        const int64_t s = n->nominalStartMs + streamOff + n->segmentOffsetMs;
        const int64_t e = n->effectiveEndMs() + streamOff + n->segmentOffsetMs;
        if (s > wallTimeMs) {
            info.hasNext = true;
            info.nextStartWallMs = s;
            info.nextJumpWallMs = std::min(std::max(s, e - 300), s + 500);
            break;
        }
    }

    return info;
}

bool SyncScheduler::hasAnyStreamAt(int64_t wallTimeMs) const {
    for (int i = 0; i < static_cast<int>(m_segmentsByStream.size()); ++i) {
        std::optional<SegmentRecord> cur, next;
        findSegmentCovering(i, wallTimeMs, cur, next);
        if (cur && cur->status != SegmentStatus::Corrupt && cur->status != SegmentStatus::Missing) {
            return true;
        }
    }
    return false;
}

std::optional<int64_t> SyncScheduler::findNearestTimeInDay(int64_t dayStartMs, int64_t preferredWallMs) const {
    const int64_t dayEndMs = dayStartMs + 86400LL * 1000LL;
    std::shared_lock lk(m_indexMtx);

    int64_t bestDist = std::numeric_limits<int64_t>::max();
    std::optional<int64_t> bestTarget;

    for (size_t i = 0; i < m_segmentsByStream.size(); ++i) {
        const int64_t off = (i < m_streamOffsetsMs.size()) ? m_streamOffsetsMs[i] : 0;
        const auto& vec = m_segmentsByStream[i];
        auto it = std::lower_bound(
            vec.begin(), vec.end(), dayStartMs - off - 10800000LL,
            [](const SegmentRecord& r, int64_t t) { return r.nominalStartMs < t; });

        for (; it != vec.end(); ++it) {
            if (it->status == SegmentStatus::Corrupt || it->status == SegmentStatus::Missing) continue;
            const int64_t s = it->nominalStartMs + off + it->segmentOffsetMs;
            const int64_t e = it->effectiveEndMs() + off + it->segmentOffsetMs;
            if (s >= dayEndMs) break;
            if (e <= dayStartMs) continue;

            const int64_t clampedStart = std::max(s, dayStartMs);
            const int64_t clampedEnd = std::min(e, dayEndMs - 1);
            if (clampedEnd < clampedStart) continue;

            int64_t candidate = preferredWallMs;
            int64_t dist = 0;
            if (preferredWallMs < clampedStart) {
                dist = clampedStart - preferredWallMs;
                candidate = std::min(clampedEnd, clampedStart + 500);
            } else if (preferredWallMs > clampedEnd) {
                dist = preferredWallMs - clampedEnd;
                candidate = std::max(clampedStart, clampedEnd - 3000);
            }

            if (dist < bestDist) {
                bestDist = dist;
                bestTarget = candidate;
                if (bestDist == 0) return bestTarget;
            }
        }
    }
    return bestTarget;
}

QVector<int> SyncScheduler::dayCoverageSlots(int64_t dayStartMs, int slotCount) const {
    QVector<int> counts(slotCount, 0);
    if (slotCount <= 0) return counts;

    const int64_t dayEndMs = dayStartMs + 86400LL * 1000LL;
    const double slotMs = 86400000.0 / static_cast<double>(slotCount);
    std::shared_lock lk(m_indexMtx);

    QVector<uint8_t> coveredMask(slotCount, 0);
    for (size_t i = 0; i < m_segmentsByStream.size() && i < 8; ++i) {
        const uint8_t bit = static_cast<uint8_t>(1U << i);
        const int64_t off = m_streamOffsetsMs[i];
        const auto& vec = m_segmentsByStream[i];
        auto it = std::lower_bound(
            vec.begin(), vec.end(), dayStartMs - off - 10800000LL,
            [](const SegmentRecord& r, int64_t t) { return r.nominalStartMs < t; });

        for (; it != vec.end(); ++it) {
            if (it->status == SegmentStatus::Corrupt || it->status == SegmentStatus::Missing) continue;
            const int64_t s = it->nominalStartMs + off + it->segmentOffsetMs;
            const int64_t e = it->effectiveEndMs() + off + it->segmentOffsetMs;
            if (s >= dayEndMs) break;
            if (e <= dayStartMs) continue;

            const int idx0 = std::clamp(static_cast<int>(std::floor((std::max(s, dayStartMs) - dayStartMs) / slotMs)), 0, slotCount - 1);
            const int idx1 = std::clamp(static_cast<int>(std::floor((std::min(e, dayEndMs - 1) - dayStartMs) / slotMs)), 0, slotCount - 1);
            for (int k = idx0; k <= idx1; ++k) {
                coveredMask[k] |= bit;
            }
        }
    }

    for (int k = 0; k < slotCount; ++k) {
        uint8_t m = coveredMask[k];
        int c = 0;
        while (m) {
            c += (m & 1U);
            m >>= 1U;
        }
        counts[k] = c;
    }
    return counts;
}

QVector<StreamDiagnostics> SyncScheduler::allDiagnostics() const {
    QVector<StreamDiagnostics> out;
    const int64_t masterMs = m_clock.currentTimeMs();
    out.reserve(static_cast<qsizetype>(m_decoders.size()));
    for (const auto& d : m_decoders) {
        out.append(d->diagnostics(masterMs));
    }
    return out;
}

std::shared_ptr<const DecodedFrame> SyncScheduler::latestFrame(int streamIndex) const {
    if (streamIndex < 0 || streamIndex >= static_cast<int>(m_decoders.size())) return nullptr;
    return m_decoders[static_cast<size_t>(streamIndex)]->latestPresentedFrame();
}

} // namespace mcs
