#pragma once

#include "core/AppConfig.h"
#include "core/Types.h"
#include "decode/AudioPlayer.h"
#include "decode/StreamDecoder.h"
#include "sync/MasterClock.h"
#include "sync/TimestampOcr.h"

#include <QObject>
#include <QTimer>
#include <QVector>
#include <functional>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <vector>

namespace mcs {

class IndexDatabase;
class NasBlockCache;

struct CalibrationVerifyResult {
    int streamIndex = 0;
    bool ok = false;
    OcrResult ocr;
    int64_t nominalTimeMs = 0;
    int64_t suggestedOffsetMs = 0;
    int64_t currentOffsetMs = 0;
};

struct NeighborSegmentInfo {
    bool hasPrev = false;
    int64_t prevEndWallMs = 0;
    int64_t prevJumpWallMs = 0;
    bool hasNext = false;
    int64_t nextStartWallMs = 0;
    int64_t nextJumpWallMs = 0;
};

class SyncScheduler : public QObject {
    Q_OBJECT
public:
    using FrameSink = std::function<void(int streamIndex,
                                         const std::shared_ptr<const DecodedFrame>&,
                                         DecodeState state,
                                         int64_t waitDurationMs)>;

    explicit SyncScheduler(IndexDatabase* db, NasBlockCache* cache, QObject* parent = nullptr);
    ~SyncScheduler() override;

    void initialize(const AppConfig& cfg);
    void shutdown();
    void reloadIndex();
    void setIndexingActive(bool active);
    void updateStreamDisplayNames(const QVector<StreamConfig>& streams);

    MasterClock* clock() { return &m_clock; }
    const MasterClock* clock() const { return &m_clock; }
    AudioPlayer* audioPlayer() { return &m_audio; }

    void setFrameSink(FrameSink sink) { m_frameSink = std::move(sink); }

    // 播放控制
    void seekTo(int64_t wallTimeMs);
    void jumpToNextSegment();
    void jumpToPrevSegment();
    void selectAudioStream(int streamIndex); // -1 全部静音
    int selectedAudioStream() const { return m_audioStreamIndex; }
    void setPreferHwDecode(bool preferHw);

    // 校准
    void setStreamOffsetMs(int streamIndex, int64_t offsetMs);
    int64_t streamOffsetMs(int streamIndex) const;
    QVector<CalibrationVerifyResult> verifyCurrentFramesOcr(bool autoApply = false);

    // 时间区间查询（供时间轴、日期导航与无录像定位使用）
    int64_t globalMinTimeMs() const;
    int64_t globalMaxTimeMs() const;
    QVector<TimeInterval> intervalsInRange(int streamIndex, int64_t startMs, int64_t endMs) const;
    QVector<int64_t> availableDayStartsMs(int tzOffsetSec) const;
    std::optional<SegmentRecord> segmentAt(int streamIndex, int64_t wallTimeMs) const;
    NeighborSegmentInfo neighborSegmentsAt(int streamIndex, int64_t wallTimeMs) const;
    bool hasAnyStreamAt(int64_t wallTimeMs) const;
    std::optional<int64_t> findNearestTimeInDay(int64_t dayStartMs, int64_t preferredWallMs) const;
    QVector<int> dayCoverageSlots(int64_t dayStartMs, int slotCount = 96) const;

    // 诊断与状态
    QVector<StreamDiagnostics> allDiagnostics() const;
    std::shared_ptr<const DecodedFrame> latestFrame(int streamIndex) const;
    int streamCount() const { return static_cast<int>(m_decoders.size()); }
    const QVector<StreamConfig>& streamConfigs() const { return m_streams; }

signals:
    void indexReloaded();
    void diagnosticsUpdated();
    void audioStreamChanged(int streamIndex);
    void streamOffsetChanged(int streamIndex, int64_t offsetMs);

private slots:
    void onTick();
    void onScrubPreview(int64_t targetMs);
    void onExactSeek(int64_t targetMs, bool resumePlaying);

private:
    void findSegmentCovering(int streamIndex,
                             int64_t wallTimeMs,
                             std::optional<SegmentRecord>& outCur,
                             std::optional<SegmentRecord>& outNext) const;
    void dispatchSeek(int64_t targetMs, bool isScrubbing);

    IndexDatabase* m_db = nullptr;
    NasBlockCache* m_cache = nullptr;
    AppConfig m_cfg;
    QVector<StreamConfig> m_streams;

    MasterClock m_clock;
    AudioPlayer m_audio;
    QTimer* m_tickTimer = nullptr;
    QTimer* m_diagTimer = nullptr;

    mutable std::shared_mutex m_indexMtx;
    std::vector<std::vector<SegmentRecord>> m_segmentsByStream;
    std::vector<int64_t> m_streamOffsetsMs;
    int64_t m_minTimeMs = 0;
    int64_t m_maxTimeMs = 0;

    std::vector<std::unique_ptr<StreamDecoder>> m_decoders;
    std::vector<std::shared_ptr<const DecodedFrame>> m_lastDelivered;
    std::vector<DecodeState> m_lastDeliveredState;
    std::vector<int64_t> m_lastDeliveredWaitMs;
    FrameSink m_frameSink;

    int m_audioStreamIndex = -1;
};

} // namespace mcs
