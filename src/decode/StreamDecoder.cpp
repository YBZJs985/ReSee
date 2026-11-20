#include "decode/StreamDecoder.h"
#include "cache/NasBlockCache.h"
#include "core/Logger.h"
#include "core/TimeUtils.h"
#include "decode/AudioPlayer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

extern "C" {
#include <libavutil/mathematics.h>
}

namespace mcs {

StreamDecoder::StreamDecoder(int streamIndex, NasBlockCache* cache, AudioPlayer* audioPlayer)
    : m_streamIndex(streamIndex), m_cache(cache), m_audioPlayer(audioPlayer), m_source(cache) {}

StreamDecoder::~StreamDecoder() {
    stop();
}

void StreamDecoder::start(const StreamConfig& cfg, bool preferHw) {
    stop();
    m_cfg = cfg;
    m_preferHw.store(preferHw, std::memory_order_relaxed);
    m_streamOffsetMs.store(cfg.defaultOffsetMs, std::memory_order_relaxed);
    m_stopRequested.store(false, std::memory_order_release);
    m_fpsWindowStartMs = TimeUtils::nowEpochMs();
    m_thread = std::jthread([this]() { workerLoop(); });
}

void StreamDecoder::stop() {
    m_stopRequested.store(true, std::memory_order_release);
    m_seekGen.fetch_add(1, std::memory_order_acq_rel);
    m_ctrlCv.notify_all();
    m_outCv.notify_all();
    if (m_thread.joinable()) {
        m_thread.join();
    }
    m_source.close();
    closeCodec();
    closeAudioCodec();
    m_hwCtx.release();
}

void StreamDecoder::setStreamOffsetMs(int64_t offsetMs) {
    m_streamOffsetMs.store(offsetMs, std::memory_order_relaxed);
}

void StreamDecoder::setAudioSelected(bool selected) {
    m_audioSelected.store(selected, std::memory_order_relaxed);
}

void StreamDecoder::setPreferHwDecode(bool preferHw) {
    m_preferHw.store(preferHw, std::memory_order_relaxed);
}

void StreamDecoder::setIndexingActive(bool active) {
    m_indexingActive.store(active, std::memory_order_release);
}

void StreamDecoder::setDisplayName(const QString& name) {
    std::lock_guard lk(m_diagMtx);
    m_cfg.displayName = name;
}

void StreamDecoder::requestSeek(const std::optional<SegmentRecord>& seg,
                                const std::optional<SegmentRecord>& nextSeg,
                                int64_t targetWallMs,
                                bool isScrubbing) {
    const uint64_t gen = m_seekGen.fetch_add(1, std::memory_order_acq_rel) + 1;
    {
        std::lock_guard lk(m_outMtx);
        m_frameQueue.clear();
        if (!seg) {
            m_lastPresented.reset();
        }
    }
    {
        std::lock_guard lk(m_diagMtx);
        if (!seg) {
            m_state = m_indexingActive.load(std::memory_order_acquire) ? DecodeState::Indexing : DecodeState::NoVideo;
            m_lastFrameWallMs = 0;
            m_lastDecodedWallMs = 0;
            m_waitStartMs = 0;
        } else if (!isScrubbing) {
            m_state = DecodeState::Locating;
            m_waitStartMs = TimeUtils::nowEpochMs();
        } else if (m_state != DecodeState::Preview && m_state != DecodeState::Playing) {
            m_state = DecodeState::Locating;
            if (m_waitStartMs == 0) {
                m_waitStartMs = TimeUtils::nowEpochMs();
            }
        }
        m_errorMsg.clear();
    }
    {
        std::lock_guard lk(m_ctrlMtx);
        SeekCommand cmd;
        cmd.gen = gen;
        cmd.seg = seg;
        cmd.nextSeg = nextSeg;
        cmd.targetWallMs = targetWallMs;
        cmd.isScrubbing = isScrubbing;
        m_pendingSeek = std::move(cmd);
        m_masterWallMs = targetWallMs;
        m_inScrubMode.store(isScrubbing, std::memory_order_relaxed);
        m_ctxSeg = seg;
        m_ctxNextSeg = nextSeg;
    }
    m_ctrlCv.notify_all();
    m_outCv.notify_all();
}

void StreamDecoder::updatePlaybackContext(int64_t masterWallMs,
                                          double playbackRate,
                                          bool playing,
                                          const std::optional<SegmentRecord>& currentSeg,
                                          const std::optional<SegmentRecord>& nextSeg) {
    std::lock_guard lk(m_ctrlMtx);
    m_masterWallMs = masterWallMs;
    m_playbackRate = playbackRate;
    m_playing = playing;
    m_ctxSeg = currentSeg;
    m_ctxNextSeg = nextSeg;
    m_ctrlCv.notify_one();
}

void StreamDecoder::clearQueueAndSetState(DecodeState st, const QString& err) {
    {
        std::lock_guard lk(m_outMtx);
        m_frameQueue.clear();
        if (st == DecodeState::NoVideo || st == DecodeState::Corrupt || st == DecodeState::Disconnected) {
            m_lastPresented.reset();
        }
    }
    {
        std::lock_guard lk(m_diagMtx);
        m_state = st;
        m_errorMsg = err;
        if (st == DecodeState::NoVideo || st == DecodeState::Corrupt || st == DecodeState::Disconnected || st == DecodeState::Playing) {
            m_waitStartMs = 0;
        }
        if (st == DecodeState::NoVideo || st == DecodeState::Corrupt || st == DecodeState::Disconnected) {
            m_lastFrameWallMs = 0;
        }
    }
}

std::shared_ptr<const DecodedFrame> StreamDecoder::consumeFrameForTime(int64_t masterWallMs, bool playing) {
    std::shared_ptr<const DecodedFrame> selected;
    bool poppedAny = false;
    bool queueEmptyAfter = false;

    {
        std::lock_guard lk(m_outMtx);
        if (m_frameQueue.empty()) {
            queueEmptyAfter = true;
        } else {
            if (!playing) {
                // 暂停或穿梭模式：直接取最新帧
                selected = m_frameQueue.back();
                m_frameQueue.clear();
                poppedAny = true;
            } else {
                // 正常播放模式：取不超过 masterWallMs + 25ms 的最新帧；
                // 若当前尚无呈现帧且队头略超前 (<400ms)，先展示首帧减少首屏等待
                while (!m_frameQueue.empty()) {
                    const auto& front = m_frameQueue.front();
                    if (front->wallTimeMs <= masterWallMs + 25) {
                        selected = front;
                        m_frameQueue.pop_front();
                        poppedAny = true;
                    } else {
                        break;
                    }
                }
                if (!selected && !m_lastPresented && !m_frameQueue.empty()) {
                    if (std::abs(m_frameQueue.front()->wallTimeMs - masterWallMs) <= 400) {
                        selected = m_frameQueue.front();
                        m_frameQueue.pop_front();
                        poppedAny = true;
                    }
                }
            }

            if (selected) {
                m_lastPresented = selected;
            }
            queueEmptyAfter = m_frameQueue.empty();
        }
    }

    if (poppedAny) {
        m_outCv.notify_one();
    }

    if (selected) {
        std::lock_guard lk(m_diagMtx);
        m_lastFrameWallMs = selected->wallTimeMs;
        m_state = selected->isPreview ? DecodeState::Preview : DecodeState::Playing;
        m_waitStartMs = 0;
    } else if (playing && m_completedSeekGen.load(std::memory_order_acquire) >= m_seekGen.load(std::memory_order_acquire)) {
        std::lock_guard lk(m_diagMtx);
        if ((m_state == DecodeState::Playing || m_state == DecodeState::Buffering) &&
            queueEmptyAfter &&
            (m_lastFrameWallMs == 0 || (masterWallMs - m_lastFrameWallMs) > 950)) {
            m_state = DecodeState::Buffering;
            if (m_waitStartMs == 0) {
                m_waitStartMs = TimeUtils::nowEpochMs();
            }
        }
    }

    return m_lastPresented;
}

std::shared_ptr<const DecodedFrame> StreamDecoder::latestPresentedFrame() const {
    std::lock_guard lk(m_outMtx);
    return m_lastPresented;
}

StreamDiagnostics StreamDecoder::diagnostics(int64_t masterWallMs) const {
    StreamDiagnostics d;
    d.streamIndex = m_streamIndex;
    d.streamKey = m_cfg.streamKey;
    d.displayName = m_cfg.displayName;
    d.appliedOffsetMs = m_streamOffsetMs.load(std::memory_order_relaxed);
    d.hwAccelActive = m_hwActive.load(std::memory_order_relaxed);

    {
        std::lock_guard lk(m_outMtx);
        d.queueDepth = static_cast<int>(m_frameQueue.size());
    }
    {
        std::lock_guard lk(m_diagMtx);
        d.hwBackend = d.hwAccelActive ? m_hwBackendName : QStringLiteral("SW");
        d.state = m_state;
        if (m_completedSeekGen.load(std::memory_order_acquire) < m_seekGen.load(std::memory_order_acquire) &&
            !m_inScrubMode.load(std::memory_order_relaxed) &&
            d.state != DecodeState::NoVideo &&
            d.state != DecodeState::Corrupt &&
            d.state != DecodeState::Disconnected) {
            d.state = DecodeState::Locating;
        }
        d.currentWallTimeMs = m_lastFrameWallMs;
        d.hasValidTime = (d.state == DecodeState::Playing && m_lastFrameWallMs > 0);
        d.hasAudioTrack = m_hasAudioTrack;
        d.syncErrorMs = d.hasValidTime ? (m_lastFrameWallMs - masterWallMs) : 0;
        if ((d.state == DecodeState::Buffering || d.state == DecodeState::Locating) && m_waitStartMs > 0) {
            d.waitDurationMs = std::max<int64_t>(0, TimeUtils::nowEpochMs() - m_waitStartMs);
        }
        d.decodedFps = m_decodedFps;
        d.lastSeekMs = m_lastSeekMs;
        d.lastNasReadMs = m_lastNasIoMs;
        d.width = m_width;
        d.height = m_height;
        d.errorMessage = m_errorMsg;
    }
    {
        std::lock_guard lk(m_ctrlMtx);
        if (m_ctxSeg) {
            d.currentFile = m_ctxSeg->path;
            d.calibConfidence = m_ctxSeg->calibConfidence;
        }
    }
    return d;
}

void StreamDecoder::closeCodec() {
    if (m_codecCtx) {
        avcodec_free_context(&m_codecCtx);
        m_codecCtx = nullptr;
    }
    m_lastCodecId = AV_CODEC_ID_NONE;
    m_lastWidth = 0;
    m_lastHeight = 0;
    m_lastExtradata.clear();
}

void StreamDecoder::closeAudioCodec() {
    if (m_audioCodecCtx) {
        avcodec_free_context(&m_audioCodecCtx);
        m_audioCodecCtx = nullptr;
    }
}

bool StreamDecoder::ensureDecoderForCodecPar(const AVCodecParameters* par, bool tryHw) {
    if (!par) return false;

    std::vector<uint8_t> extra;
    if (par->extradata && par->extradata_size > 0) {
        extra.assign(par->extradata, par->extradata + par->extradata_size);
    }

    const bool wantHw = tryHw && m_hwCtx.isHardwareReady();
    const bool sameParams =
        m_codecCtx &&
        m_lastCodecId == par->codec_id &&
        m_lastWidth == par->width &&
        m_lastHeight == par->height &&
        m_lastExtradata == extra &&
        (m_hwActive.load(std::memory_order_relaxed) == wantHw);

    if (sameParams) {
        avcodec_flush_buffers(m_codecCtx);
        return true;
    }

    closeCodec();
    const AVCodec* codec = avcodec_find_decoder(par->codec_id);
    if (!codec) return false;

    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx) return false;

    if (avcodec_parameters_to_context(m_codecCtx, par) < 0) {
        closeCodec();
        return false;
    }

    m_codecCtx->thread_count = wantHw ? 2 : 4;
    bool usingHw = false;
    if (wantHw) {
        m_hwCtx.attachToCodec(m_codecCtx);
        usingHw = true;
    }
    m_hwActive.store(usingHw, std::memory_order_relaxed);

    if (avcodec_open2(m_codecCtx, codec, nullptr) < 0) {
        if (usingHw) {
            MCS_LOG_WARN("DECODE", QStringLiteral("流 %1 硬解打开失败，降级软解").arg(m_cfg.streamKey));
            m_hwActive.store(false, std::memory_order_relaxed);
            closeCodec();
            return ensureDecoderForCodecPar(par, false);
        }
        closeCodec();
        return false;
    }

    m_lastCodecId = par->codec_id;
    m_lastWidth = par->width;
    m_lastHeight = par->height;
    m_lastExtradata = std::move(extra);
    return true;
}

void StreamDecoder::maybePrefetchNextSegment(const std::optional<SegmentRecord>& nextSeg) {
    if (!nextSeg || nextSeg->status != SegmentStatus::Ok) return;
    if (m_prefetchedPath == nextSeg->path) return;
    m_prefetchedPath = nextSeg->path;
    // 通过 NasBlockCache 的受控预读队列后台拉取下个切片前 2MB（覆盖头+sidx+首片）
    if (m_cache) {
        m_cache->enqueuePrefetch(nextSeg->path, static_cast<uint64_t>(nextSeg->fileSize),
                                 0, NasBlockCache::kChunkSize);
    }
}

void StreamDecoder::workerLoop() {
    const bool wantHw = m_preferHw.load(std::memory_order_relaxed);
    m_hwCtx.init(wantHw);
    {
        std::lock_guard lk(m_diagMtx);
        m_hwBackendName = m_hwCtx.backendName();
    }

    while (!m_stopRequested.load(std::memory_order_acquire)) {
        std::optional<SeekCommand> seekCmd;
        bool playing = false;
        std::optional<SegmentRecord> ctxSeg;
        std::optional<SegmentRecord> ctxNextSeg;
        int64_t masterWallMs = 0;

        {
            std::unique_lock lk(m_ctrlMtx);
            if (m_pendingSeek) {
                seekCmd = std::move(m_pendingSeek);
                m_pendingSeek.reset();
            } else {
                playing = m_playing;
                ctxSeg = m_ctxSeg;
                ctxNextSeg = m_ctxNextSeg;
                masterWallMs = m_masterWallMs;
            }
        }

        if (seekCmd) {
            handleSeekCommand(*seekCmd);
            continue;
        }

        if (!playing) {
            std::unique_lock lk(m_ctrlMtx);
            if (!m_pendingSeek && !m_playing && !m_stopRequested.load(std::memory_order_acquire)) {
                m_ctrlCv.wait_for(lk, std::chrono::milliseconds(50));
            }
            continue;
        }

        // 正在播放：若上下文指示无录像，清空画面（索引扫描尚未完成时显示“索引中”）
        if (!ctxSeg) {
            m_source.close();
            m_openedSeg.reset();
            clearQueueAndSetState(m_indexingActive.load(std::memory_order_acquire)
                                      ? DecodeState::Indexing
                                      : DecodeState::NoVideo);
            std::unique_lock lk(m_ctrlMtx);
            m_ctrlCv.wait_for(lk, std::chrono::milliseconds(40));
            continue;
        }

        // 检查是否需要切换文件（进入新切片或从空洞恢复）
        const bool needOpenNew = !m_openedSeg || m_openedSeg->path != ctxSeg->path;
        if (needOpenNew) {
            if (m_reconnectWaitUntilMs > 0 && TimeUtils::nowEpochMs() < m_reconnectWaitUntilMs) {
                std::unique_lock lk(m_ctrlMtx);
                m_ctrlCv.wait_for(lk, std::chrono::milliseconds(80));
                continue;
            }
            SeekCommand autoSeek;
            autoSeek.gen = m_seekGen.load(std::memory_order_acquire);
            autoSeek.seg = ctxSeg;
            autoSeek.nextSeg = ctxNextSeg;
            autoSeek.targetWallMs = masterWallMs;
            autoSeek.isScrubbing = false;
            handleSeekCommand(autoSeek);
            continue;
        }

        // 检查当前切片是否到达末尾
        if (m_openedEof) {
            if (ctxNextSeg) {
                const int64_t totalOff = m_streamOffsetMs.load(std::memory_order_relaxed) + ctxNextSeg->segmentOffsetMs;
                const int64_t nextStartWallMs = ctxNextSeg->nominalStartMs + totalOff;
                if (masterWallMs >= nextStartWallMs - 200) {
                    SeekCommand nextCmd;
                    nextCmd.gen = m_seekGen.load(std::memory_order_acquire);
                    nextCmd.seg = ctxNextSeg;
                    nextCmd.nextSeg = std::nullopt;
                    nextCmd.targetWallMs = std::max(masterWallMs, nextStartWallMs);
                    nextCmd.isScrubbing = false;
                    handleSeekCommand(nextCmd);
                    continue;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        // 队列已足够且超前主时钟时礼貌等待消费者消化
        {
            std::unique_lock lk(m_outMtx);
            if (m_frameQueue.size() >= kMaxQueueFrames &&
                m_frameQueue.back()->wallTimeMs > masterWallMs + 180) {
                m_outCv.wait_for(lk, std::chrono::milliseconds(15));
                continue;
            }
        }

        stepDecodeLoop();
    }
}

bool StreamDecoder::handleSeekCommand(const SeekCommand& cmd) {
    const auto t0 = std::chrono::steady_clock::now();

    if (!cmd.seg) {
        m_source.close();
        m_openedSeg.reset();
        clearQueueAndSetState(m_indexingActive.load(std::memory_order_acquire)
                                  ? DecodeState::Indexing
                                  : DecodeState::NoVideo);
        m_completedSeekGen.store(cmd.gen, std::memory_order_release);
        return false;
    }

    const SegmentRecord& seg = *cmd.seg;
    if (seg.status == SegmentStatus::Corrupt) {
        m_source.close();
        m_openedSeg = seg;
        m_openedEof = true;
        clearQueueAndSetState(DecodeState::Corrupt, QStringLiteral("录像文件损坏"));
        m_completedSeekGen.store(cmd.gen, std::memory_order_release);
        return false;
    }

    if (!m_cache->isFileReachable(seg.path)) {
        m_source.close();
        m_openedSeg.reset();
        m_reconnectWaitUntilMs = TimeUtils::nowEpochMs() + 1500;
        clearQueueAndSetState(DecodeState::Disconnected, QStringLiteral("NAS 文件不可访问"));
        m_completedSeekGen.store(cmd.gen, std::memory_order_release);
        return false;
    }
    m_reconnectWaitUntilMs = 0;

    {
        std::lock_guard lk(m_diagMtx);
        if (m_state != DecodeState::Playing && m_state != DecodeState::Preview) {
            m_state = DecodeState::Buffering;
        }
    }

    const int64_t totalOffsetMs = m_streamOffsetMs.load(std::memory_order_relaxed) + seg.segmentOffsetMs;
    const double targetSecInFile = std::max(0.0, static_cast<double>(cmd.targetWallMs - seg.nominalStartMs - totalOffsetMs) / 1000.0);

    bool cancelled = false;
    const bool singleFrag = cmd.isScrubbing;
    if (!m_source.open(seg, targetSecInFile, singleFrag, &m_seekGen, cmd.gen, &cancelled)) {
        if (cancelled || m_seekGen.load(std::memory_order_acquire) != cmd.gen) {
            return false; // 已被更新的 Seek 取代，安静退出
        }
        m_openedSeg = seg;
        m_openedEof = true;
        clearQueueAndSetState(DecodeState::Corrupt, QStringLiteral("打开或解析录像头部失败"));
        m_completedSeekGen.store(cmd.gen, std::memory_order_release);
        return false;
    }

    m_openedSeg = seg;
    m_openedEof = false;
    m_ptsCorrectionUs = 0;
    m_checkedPtsCorrection = false;

    AVFormatContext* fmt = m_source.formatContext();
    const int vIdx = m_source.videoStreamIndex();
    const AVCodecParameters* vPar = fmt->streams[vIdx]->codecpar;
    if (!ensureDecoderForCodecPar(vPar, m_preferHw.load(std::memory_order_relaxed))) {
        m_openedEof = true;
        clearQueueAndSetState(DecodeState::Corrupt, QStringLiteral("不支持的视频编码格式"));
        m_completedSeekGen.store(cmd.gen, std::memory_order_release);
        return false;
    }
    {
        std::lock_guard lk(m_diagMtx);
        m_hwBackendName = m_hwCtx.backendName();
    }

    closeAudioCodec();
    m_hasAudioTrack = false;
    const int aIdx = m_source.audioStreamIndex();
    if (aIdx >= 0) {
        const AVCodecParameters* aPar = fmt->streams[aIdx]->codecpar;
        if (const AVCodec* ac = avcodec_find_decoder(aPar->codec_id)) {
            m_audioCodecCtx = avcodec_alloc_context3(ac);
            if (!m_audioCodecCtx ||
                avcodec_parameters_to_context(m_audioCodecCtx, aPar) < 0 ||
                avcodec_open2(m_audioCodecCtx, ac, nullptr) < 0) {
                closeAudioCodec();
            } else {
                m_hasAudioTrack = true;
            }
        }
    }

    m_codecCtx->skip_frame = AVDISCARD_DEFAULT;
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();

    std::shared_ptr<DecodedFrame> bestFrame;
    const int64_t targetPtsUs = static_cast<int64_t>(targetSecInFile * 1'000'000.0);
    const AVRational tb = fmt->streams[vIdx]->time_base;
    const bool quickFirstOnly = cmd.isScrubbing || cmd.keyframeOnlyFirstFrame;

    int framesDecoded = 0;
    while (!m_stopRequested.load(std::memory_order_acquire)) {
        if (m_seekGen.load(std::memory_order_acquire) != cmd.gen) {
            cancelled = true;
            break;
        }

        const int r = av_read_frame(fmt, pkt);
        if (r < 0) {
            if (r == AVERROR_EXIT || m_seekGen.load(std::memory_order_acquire) != cmd.gen) {
                cancelled = true;
            }
            break;
        }

        if (pkt->stream_index != vIdx) {
            av_packet_unref(pkt);
            continue;
        }

        if (avcodec_send_packet(m_codecCtx, pkt) < 0) {
            av_packet_unref(pkt);
            continue;
        }
        av_packet_unref(pkt);

        while (true) {
            const int rf = avcodec_receive_frame(m_codecCtx, frame);
            if (rf == AVERROR(EAGAIN) || rf == AVERROR_EOF) break;
            if (rf < 0) break;

            int64_t rawPts = (frame->best_effort_timestamp != AV_NOPTS_VALUE) ? frame->best_effort_timestamp : frame->pts;
            if (rawPts == AV_NOPTS_VALUE) rawPts = 0;
            int64_t ptsUs = av_rescale_q(rawPts, tb, {1, 1000000});

            if (!m_checkedPtsCorrection) {
                m_checkedPtsCorrection = true;
                if (m_source.isFragmented() && m_source.openedFragmentStartSec() > 2.0 &&
                    ptsUs < static_cast<int64_t>((m_source.openedFragmentStartSec() - 1.5) * 1'000'000.0)) {
                    m_ptsCorrectionUs = static_cast<int64_t>(m_source.openedFragmentStartSec() * 1'000'000.0);
                }
            }
            ptsUs += m_ptsCorrectionUs;

            ++framesDecoded;

            // 穿梭/关键帧步进模式：只解首帧关键帧立即交付，或者精确模式下找到到达目标时间点的帧（允许 10ms 微小帧率容差）
            const bool reachedTarget = quickFirstOnly || (ptsUs >= targetPtsUs - 10000);
            if (reachedTarget || framesDecoded == 1) {
                auto df = std::make_shared<DecodedFrame>();
                bool isHw = false;
                if (m_hwCtx.extractNv12Frame(frame, df->width, df->height, df->yStride, df->uvStride, df->nv12Data, isHw)) {
                    df->streamIndex = m_streamIndex;
                    df->ptsUs = ptsUs;
                    df->nominalTimeMs = seg.nominalStartMs + (ptsUs / 1000);
                    df->wallTimeMs = df->nominalTimeMs + totalOffsetMs;
                    df->isPreview = cmd.isScrubbing && m_inScrubMode.load(std::memory_order_relaxed);
                    df->isHardware = isHw;
                    df->seekGen = cmd.gen;
                    df->segmentPath = seg.path;
                    bestFrame = std::move(df);
                }
            }
            av_frame_unref(frame);

            if (quickFirstOnly && bestFrame) {
                break;
            }
            if (!quickFirstOnly && ptsUs >= targetPtsUs - 10000 && bestFrame) {
                break;
            }
            // 安全上限：防止单个 fragment 异常过多帧阻塞太久
            if (framesDecoded > 260) {
                break;
            }
        }

        if (bestFrame && (quickFirstOnly || bestFrame->ptsUs >= targetPtsUs - 10000 || framesDecoded > 260)) {
            break;
        }
    }

    av_frame_free(&frame);
    av_packet_free(&pkt);

    if (cancelled || m_seekGen.load(std::memory_order_acquire) != cmd.gen) {
        return false;
    }

    const double elapsedMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();

    if (bestFrame) {
        {
            std::lock_guard lk(m_outMtx);
            m_frameQueue.clear();
            m_frameQueue.push_back(bestFrame);
            m_lastPresented = bestFrame;
        }
        {
            std::lock_guard lk(m_diagMtx);
            m_lastSeekMs = elapsedMs;
            m_lastNasIoMs = m_source.lastNasIoMs();
            m_lastFrameWallMs = bestFrame->wallTimeMs;
            m_lastDecodedWallMs = bestFrame->wallTimeMs;
            m_width = bestFrame->width;
            m_height = bestFrame->height;
            // 如果当前帧来自高倍速 4K 步进追赶，状态应仍为已同步播放而非拖拽预览
            const bool treatAsPreview = cmd.isScrubbing && m_inScrubMode.load(std::memory_order_relaxed);
            m_state = treatAsPreview ? DecodeState::Preview : DecodeState::Playing;
            m_waitStartMs = 0;
            m_errorMsg.clear();
        }
        m_completedSeekGen.store(cmd.gen, std::memory_order_release);
        maybePrefetchNextSegment(cmd.nextSeg);
        return true;
    }

    m_openedEof = true;
    clearQueueAndSetState(DecodeState::Corrupt, QStringLiteral("Seek 未解码出有效画面"));
    m_completedSeekGen.store(cmd.gen, std::memory_order_release);
    return false;
}

void StreamDecoder::stepDecodeLoop() {
    if (!m_source.formatContext() || !m_codecCtx || !m_openedSeg) return;

    int64_t masterWallMs = 0;
    double rate = 1.0;
    std::optional<SegmentRecord> nextSeg;
    {
        std::lock_guard lk(m_ctrlMtx);
        masterWallMs = m_masterWallMs;
        rate = m_playbackRate;
        nextSeg = m_ctxNextSeg;
    }

    const SegmentRecord& seg = *m_openedSeg;
    const int64_t totalOffsetMs = m_streamOffsetMs.load(std::memory_order_relaxed) + seg.segmentOffsetMs;
    const int64_t refWallMs = (m_lastDecodedWallMs > 0) ? m_lastDecodedWallMs : m_lastFrameWallMs;
    const int64_t lagMs = masterWallMs - refWallMs;

    // 高倍速(>=6x) 且高分辨率(>=3000px) 时，若超过 NVDEC 吞吐而落后 >900ms，切换为关键帧直出步进策略，防止重解 P 帧陷入追赶循环
    if (rate >= 6.0 && m_lastWidth >= 3000) {
        if (lagMs > 900) {
            SeekCommand kfStep;
            kfStep.gen = m_seekGen.load(std::memory_order_acquire);
            kfStep.seg = m_openedSeg;
            kfStep.nextSeg = nextSeg;
            kfStep.targetWallMs = masterWallMs;
            // 在连续流上仅解关键帧首帧出帧（不进入拖拽预览态、保持完整片段连续读取）
            kfStep.isScrubbing = false;
            kfStep.keyframeOnlyFirstFrame = true;
            handleSeekCommand(kfStep);
            return;
        }
        if (lagMs < -200) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            return;
        }
    } else if (refWallMs > 0 && ((lagMs > 3000) || (rate >= 4.0 && lagMs > 1200))) {
        SeekCommand catchup;
        catchup.gen = m_seekGen.load(std::memory_order_acquire);
        catchup.seg = m_openedSeg;
        catchup.nextSeg = nextSeg;
        catchup.targetWallMs = masterWallMs;
        catchup.isScrubbing = false;
        handleSeekCommand(catchup);
        return;
    }

    // 轻度落后 (>350ms) 或 4x 以上倍速时允许解码器跳过非参考 B/P 帧加速追赶
    m_codecCtx->skip_frame = (rate >= 6.0 && m_lastWidth >= 3000)
        ? AVDISCARD_NONKEY
        : ((lagMs > 350 || rate >= 4.0) ? AVDISCARD_NONREF : AVDISCARD_DEFAULT);

    // 接近当前切片尾部（剩余 <= 25s）自动后台预热下一切片
    if (seg.effectiveEndMs() + totalOffsetMs - masterWallMs <= 25000 && nextSeg) {
        maybePrefetchNextSegment(nextSeg);
    }

    AVFormatContext* fmt = m_source.formatContext();
    const int vIdx = m_source.videoStreamIndex();
    const int aIdx = m_source.audioStreamIndex();
    const AVRational tb = fmt->streams[vIdx]->time_base;

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();

    const uint64_t startGen = m_seekGen.load(std::memory_order_acquire);
    int decodedInBurst = 0;

    while (decodedInBurst < 2 && !m_stopRequested.load(std::memory_order_acquire)) {
        if (m_seekGen.load(std::memory_order_acquire) != startGen) break;

        const int r = av_read_frame(fmt, pkt);
        if (r < 0) {
            if (r == AVERROR_EOF) {
                m_openedEof = true;
            }
            break;
        }

        if (pkt->stream_index == aIdx) {
            if (m_audioCodecCtx && m_audioPlayer && m_audioSelected.load(std::memory_order_relaxed) &&
                rate <= 2.2 && lagMs < 350) {
                bool audioStale = false;
                if (pkt->pts != AV_NOPTS_VALUE) {
                    const AVRational atb = fmt->streams[aIdx]->time_base;
                    const int64_t aPtsUs = av_rescale_q(pkt->pts, atb, {1, 1000000}) + m_ptsCorrectionUs;
                    const int64_t aWallMs = seg.nominalStartMs + (aPtsUs / 1000) + totalOffsetMs;
                    if (aWallMs < masterWallMs - 220) {
                        audioStale = true;
                    }
                }
                if (!audioStale && m_audioPlayer->queuedAudioMs() < 320 &&
                    avcodec_send_packet(m_audioCodecCtx, pkt) >= 0) {
                    while (avcodec_receive_frame(m_audioCodecCtx, frame) >= 0) {
                        m_audioPlayer->feedAudioFrame(frame);
                        av_frame_unref(frame);
                    }
                }
            }
            av_packet_unref(pkt);
            continue;
        }

        if (pkt->stream_index != vIdx) {
            av_packet_unref(pkt);
            continue;
        }

        if (avcodec_send_packet(m_codecCtx, pkt) < 0) {
            av_packet_unref(pkt);
            continue;
        }
        av_packet_unref(pkt);

        while (true) {
            const int rf = avcodec_receive_frame(m_codecCtx, frame);
            if (rf == AVERROR(EAGAIN) || rf == AVERROR_EOF) break;
            if (rf < 0) break;

            int64_t rawPts = (frame->best_effort_timestamp != AV_NOPTS_VALUE) ? frame->best_effort_timestamp : frame->pts;
            if (rawPts == AV_NOPTS_VALUE) rawPts = 0;
            int64_t ptsUs = av_rescale_q(rawPts, tb, {1, 1000000}) + m_ptsCorrectionUs;

            const int64_t nominalMs = seg.nominalStartMs + (ptsUs / 1000);
            const int64_t wallMs = nominalMs + totalOffsetMs;

            // 过时超过 180ms 的帧直接丢弃，不做 NV12 传输与拷贝（保留未呈现首帧时的保护）
            if (wallMs < masterWallMs - 180 && m_lastFrameWallMs > 0) {
                av_frame_unref(frame);
                continue;
            }

            auto df = std::make_shared<DecodedFrame>();
            bool isHw = false;
            if (m_hwCtx.extractNv12Frame(frame, df->width, df->height, df->yStride, df->uvStride, df->nv12Data, isHw)) {
                df->streamIndex = m_streamIndex;
                df->ptsUs = ptsUs;
                df->nominalTimeMs = nominalMs;
                df->wallTimeMs = wallMs;
                df->isPreview = false;
                df->isHardware = isHw;
                df->seekGen = startGen;
                df->segmentPath = seg.path;

                {
                    std::lock_guard lk(m_outMtx);
                    while (m_frameQueue.size() >= kMaxQueueFrames) {
                        m_frameQueue.pop_front();
                    }
                    m_frameQueue.push_back(std::move(df));
                }
                m_lastDecodedWallMs = wallMs;

                // 统计真实解码 FPS
                ++m_fpsCounter;
                const int64_t nowMs = TimeUtils::nowEpochMs();
                const int64_t dt = nowMs - m_fpsWindowStartMs;
                if (dt >= 1000) {
                    std::lock_guard lk(m_diagMtx);
                    m_decodedFps = static_cast<double>(m_fpsCounter) * 1000.0 / static_cast<double>(dt);
                    m_fpsCounter = 0;
                    m_fpsWindowStartMs = nowMs;
                }
                ++decodedInBurst;
            }
            av_frame_unref(frame);
        }
    }

    av_frame_free(&frame);
    av_packet_free(&pkt);
}

} // namespace mcs
