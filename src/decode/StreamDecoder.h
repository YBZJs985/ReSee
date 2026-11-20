#pragma once

#include "core/Types.h"
#include "decode/Fmp4StreamSource.h"
#include "decode/HwAccelContext.h"

#include <QMutex>
#include <QString>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace mcs {

class NasBlockCache;
class AudioPlayer;

class StreamDecoder {
public:
    StreamDecoder(int streamIndex, NasBlockCache* cache, AudioPlayer* audioPlayer);
    ~StreamDecoder();

    StreamDecoder(const StreamDecoder&) = delete;
    StreamDecoder& operator=(const StreamDecoder&) = delete;

    void start(const StreamConfig& cfg, bool preferHw);
    void stop();

    void setStreamOffsetMs(int64_t offsetMs);
    void setAudioSelected(bool selected);
    void setPreferHwDecode(bool preferHw);
    void setIndexingActive(bool active);
    void setDisplayName(const QString& name);

    // 发起 Seek（立即递增版本号使旧读取/旧解码失效）
    void requestSeek(const std::optional<SegmentRecord>& seg,
                     const std::optional<SegmentRecord>& nextSeg,
                     int64_t targetWallMs,
                     bool isScrubbing);

    // 正常播放周期由 SyncScheduler 调用，同步主时钟与当前时刻切片上下文
    void updatePlaybackContext(int64_t masterWallMs,
                               double playbackRate,
                               bool playing,
                               const std::optional<SegmentRecord>& currentSeg,
                               const std::optional<SegmentRecord>& nextSeg);

    // 由 SyncScheduler 消费已解码帧：返回应该在当前 masterWallMs 显示的帧（若队列有新帧）
    std::shared_ptr<const DecodedFrame> consumeFrameForTime(int64_t masterWallMs, bool playing);

    // 获取最新可显示帧（不移除队列），供截屏或 OCR 采样使用
    std::shared_ptr<const DecodedFrame> latestPresentedFrame() const;

    StreamDiagnostics diagnostics(int64_t masterWallMs) const;

private:
    struct SeekCommand {
        uint64_t gen = 0;
        std::optional<SegmentRecord> seg;
        std::optional<SegmentRecord> nextSeg;
        int64_t targetWallMs = 0;
        bool isScrubbing = false;
        bool keyframeOnlyFirstFrame = false;
    };

    void workerLoop();
    bool ensureDecoderForCodecPar(const AVCodecParameters* par, bool tryHw);
    void closeCodec();
    void closeAudioCodec();
    void clearQueueAndSetState(DecodeState st, const QString& err = QString());
    void maybePrefetchNextSegment(const std::optional<SegmentRecord>& nextSeg);

    bool handleSeekCommand(const SeekCommand& cmd);
    void stepDecodeLoop();

    const int m_streamIndex;
    NasBlockCache* m_cache = nullptr;
    AudioPlayer* m_audioPlayer = nullptr;

    StreamConfig m_cfg;
    std::atomic<bool> m_preferHw{true};
    std::atomic<bool> m_hwActive{false};
    std::atomic<bool> m_indexingActive{false};
    std::atomic<int64_t> m_streamOffsetMs{0};
    std::atomic<bool> m_audioSelected{false};

    // 请求版本号：任何新 Seek 立刻递增，中断正在阻塞的 I/O 与解码
    std::atomic<uint64_t> m_seekGen{0};
    std::atomic<uint64_t> m_completedSeekGen{0};
    std::atomic<bool> m_stopRequested{false};

    // 指令与播放上下文
    mutable std::mutex m_ctrlMtx;
    std::condition_variable m_ctrlCv;
    std::optional<SeekCommand> m_pendingSeek;
    int64_t m_masterWallMs = 0;
    double m_playbackRate = 1.0;
    bool m_playing = false;
    std::atomic<bool> m_inScrubMode{false};
    std::optional<SegmentRecord> m_ctxSeg;
    std::optional<SegmentRecord> m_ctxNextSeg;

    // 帧队列与状态
    mutable std::mutex m_outMtx;
    std::condition_variable m_outCv;
    std::deque<std::shared_ptr<const DecodedFrame>> m_frameQueue;
    std::shared_ptr<const DecodedFrame> m_lastPresented;
    static constexpr size_t kMaxQueueFrames = 5;

    // 解码线程内部状态
    HwAccelContext m_hwCtx;
    Fmp4StreamSource m_source;
    AVCodecContext* m_codecCtx = nullptr;
    AVCodecContext* m_audioCodecCtx = nullptr;
    AVCodecID m_lastCodecId = AV_CODEC_ID_NONE;
    int m_lastWidth = 0;
    int m_lastHeight = 0;
    std::vector<uint8_t> m_lastExtradata;

    std::optional<SegmentRecord> m_openedSeg;
    bool m_openedEof = false;
    bool m_hasAudioTrack = false;
    int64_t m_ptsCorrectionUs = 0;
    bool m_checkedPtsCorrection = false;
    QString m_prefetchedPath;

    // 诊断统计
    mutable std::mutex m_diagMtx;
    DecodeState m_state = DecodeState::Idle;
    QString m_hwBackendName = QStringLiteral("SW");
    QString m_errorMsg;
    double m_lastSeekMs = 0.0;
    double m_lastNasIoMs = 0.0;
    int64_t m_lastFrameWallMs = 0;
    int64_t m_lastDecodedWallMs = 0;
    int64_t m_waitStartMs = 0;
    int64_t m_reconnectWaitUntilMs = 0;
    int m_width = 0;
    int m_height = 0;
    double m_decodedFps = 0.0;
    uint64_t m_fpsCounter = 0;
    int64_t m_fpsWindowStartMs = 0;

    std::jthread m_thread;
};

} // namespace mcs
