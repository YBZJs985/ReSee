#include "decode/AudioPlayer.h"
#include "core/TimeUtils.h"

#include <QAudioDevice>
#include <algorithm>
#include <cmath>
#include <cstring>

extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

namespace mcs {

AudioRingDevice::AudioRingDevice(QObject* parent)
    : QIODevice(parent), m_ring(kMaxRingBytes, 0) {
    open(QIODevice::ReadOnly);
}

void AudioRingDevice::pushPcm(const uint8_t* data, size_t bytes) {
    if (!data || bytes == 0) return;
    QMutexLocker lk(&m_mtx);
    if (bytes >= kMaxRingBytes) {
        data += (bytes - kMaxRingBytes);
        bytes = kMaxRingBytes;
        m_head = 0;
        m_size = 0;
    }
    while (m_size + bytes > kMaxRingBytes) {
        const size_t drop = (m_size + bytes) - kMaxRingBytes;
        m_head = (m_head + drop) % kMaxRingBytes;
        m_size -= drop;
    }
    const size_t tail = (m_head + m_size) % kMaxRingBytes;
    const size_t firstPart = std::min(bytes, kMaxRingBytes - tail);
    std::memcpy(m_ring.data() + tail, data, firstPart);
    if (bytes > firstPart) {
        std::memcpy(m_ring.data(), data + firstPart, bytes - firstPart);
    }
    m_size += bytes;
}

void AudioRingDevice::clear() {
    QMutexLocker lk(&m_mtx);
    m_head = 0;
    m_size = 0;
}

size_t AudioRingDevice::bufferedBytes() const {
    QMutexLocker lk(&m_mtx);
    return m_size;
}

qint64 AudioRingDevice::readData(char* data, qint64 maxlen) {
    if (maxlen <= 0) return 0;
    QMutexLocker lk(&m_mtx);
    const size_t toRead = std::min(static_cast<size_t>(maxlen), m_size);
    if (toRead == 0) {
        const size_t sil = std::min(static_cast<size_t>(maxlen), size_t(1024));
        std::memset(data, 0, sil);
        return static_cast<qint64>(sil);
    }
    const size_t firstPart = std::min(toRead, kMaxRingBytes - m_head);
    std::memcpy(data, m_ring.data() + m_head, firstPart);
    if (toRead > firstPart) {
        std::memcpy(data + firstPart, m_ring.data(), toRead - firstPart);
    }
    m_head = (m_head + toRead) % kMaxRingBytes;
    m_size -= toRead;
    return static_cast<qint64>(toRead);
}

qint64 AudioRingDevice::writeData(const char*, qint64) {
    return -1;
}

qint64 AudioRingDevice::bytesAvailable() const {
    QMutexLocker lk(&m_mtx);
    return static_cast<qint64>(m_size) + QIODevice::bytesAvailable();
}

AudioPlayer::AudioPlayer(QObject* parent) : QObject(parent) {
    m_device = new AudioRingDevice(this);
    m_mediaDevices = new QMediaDevices(this);
    connect(m_mediaDevices, &QMediaDevices::audioOutputsChanged,
            this, &AudioPlayer::onAudioOutputsChanged);
}

AudioPlayer::~AudioPlayer() {
    stopOutput();
    QMutexLocker lk(&m_procMtx);
    releaseTempoFilter();
    if (m_swr) {
        swr_free(&m_swr);
    }
}

void AudioPlayer::startIfNeeded() {
    if (!m_enabled.load(std::memory_order_acquire) || m_sink) return;
    QAudioFormat fmt;
    fmt.setSampleRate(48000);
    fmt.setChannelCount(2);
    fmt.setSampleFormat(QAudioFormat::Int16);

    const QAudioDevice dev = QMediaDevices::defaultAudioOutput();
    if (dev.isNull()) return;

    m_sink = new QAudioSink(dev, fmt, this);
    m_sink->setBufferSize(48000 * 2 * 2 / 5); // 200ms 缓冲
    const double effVol = m_muted.load(std::memory_order_relaxed) ? 0.0 : m_volume.load(std::memory_order_relaxed);
    m_sink->setVolume(effVol);
    m_fadeSamplesLeft.store(4800, std::memory_order_relaxed); // 100ms 淡入
    m_sink->start(m_device);
}

void AudioPlayer::stopOutput() {
    if (m_sink) {
        m_sink->stop();
        m_sink->deleteLater();
        m_sink = nullptr;
    }
    m_device->clear();
}

void AudioPlayer::onAudioOutputsChanged() {
    if (m_sink) {
        stopOutput();
        if (m_enabled.load(std::memory_order_acquire)) {
            startIfNeeded();
        }
    }
}

void AudioPlayer::setEnabled(bool enabled) {
    bool expected = !enabled;
    if (!m_enabled.compare_exchange_strong(expected, enabled, std::memory_order_acq_rel)) return;
    if (enabled) {
        m_fadeSamplesLeft.store(4800, std::memory_order_relaxed);
        QMetaObject::invokeMethod(this, &AudioPlayer::startIfNeeded, Qt::QueuedConnection);
    } else {
        QMetaObject::invokeMethod(this, &AudioPlayer::stopOutput, Qt::QueuedConnection);
    }
}

void AudioPlayer::setVolume(double vol) {
    const double clamped = std::clamp(vol, 0.0, 1.0);
    m_volume.store(clamped, std::memory_order_relaxed);
    if (m_sink && !m_muted.load(std::memory_order_relaxed)) {
        m_sink->setVolume(clamped);
    }
}

void AudioPlayer::setMuted(bool muted) {
    const bool prev = m_muted.exchange(muted, std::memory_order_relaxed);
    if (m_sink) {
        if (muted) {
            m_sink->setVolume(0.0);
        } else {
            if (prev) {
                m_fadeSamplesLeft.store(4800, std::memory_order_relaxed);
            }
            m_sink->setVolume(m_volume.load(std::memory_order_relaxed));
        }
    }
}

void AudioPlayer::setPlaybackRate(double rate) {
    m_rate.store(std::clamp(rate, 0.25, 8.0), std::memory_order_relaxed);
}

void AudioPlayer::prepareStreamSwitch() {
    clearBuffer();
    m_fadeSamplesLeft.store(5280, std::memory_order_relaxed); // 110ms 切换平滑淡入
}

void AudioPlayer::clearBuffer() {
    m_device->clear();
    m_fadeSamplesLeft.store(4800, std::memory_order_relaxed);
    QMutexLocker lk(&m_procMtx);
    releaseTempoFilter();
}

int64_t AudioPlayer::queuedAudioMs() const {
    const size_t bytes = m_device->bufferedBytes();
    // 48000 Hz * 2 ch * 2 bytes = 192000 bytes/sec = 192 bytes/ms
    return static_cast<int64_t>(bytes / 192);
}

void AudioPlayer::ensureResampler(int srcSampleRate, int srcChannels, int srcFmt, const void* chLayoutPtr) {
    if (m_swr && m_lastRate == srcSampleRate && m_lastCh == srcChannels && m_lastFmt == srcFmt) {
        return;
    }
    if (m_swr) {
        swr_free(&m_swr);
    }
    AVChannelLayout outLayout = AV_CHANNEL_LAYOUT_STEREO;
    AVChannelLayout inLayout;
    if (chLayoutPtr) {
        av_channel_layout_copy(&inLayout, static_cast<const AVChannelLayout*>(chLayoutPtr));
    } else {
        av_channel_layout_default(&inLayout, srcChannels);
    }

    if (swr_alloc_set_opts2(&m_swr,
                            &outLayout, AV_SAMPLE_FMT_S16, 48000,
                            &inLayout, static_cast<AVSampleFormat>(srcFmt), srcSampleRate,
                            0, nullptr) >= 0) {
        if (swr_init(m_swr) < 0) {
            swr_free(&m_swr);
        }
    }
    av_channel_layout_uninit(&inLayout);
    m_lastRate = srcSampleRate;
    m_lastCh = srcChannels;
    m_lastFmt = srcFmt;
}

void AudioPlayer::releaseTempoFilter() {
    if (m_filterGraph) {
        avfilter_graph_free(&m_filterGraph);
        m_filterGraph = nullptr;
        m_abufferCtx = nullptr;
        m_abuffersinkCtx = nullptr;
    }
    m_filterRate = 1.0;
}

void AudioPlayer::ensureTempoFilter(double rate) {
    if (std::abs(rate - 1.0) <= 0.05) {
        releaseTempoFilter();
        return;
    }
    if (m_filterGraph && std::abs(m_filterRate - rate) <= 0.01) {
        return;
    }
    releaseTempoFilter();

    m_filterGraph = avfilter_graph_alloc();
    if (!m_filterGraph) return;

    const AVFilter* abuf = avfilter_get_by_name("abuffer");
    const AVFilter* atempo = avfilter_get_by_name("atempo");
    const AVFilter* asink = avfilter_get_by_name("abuffersink");
    if (!abuf || !atempo || !asink) {
        releaseTempoFilter();
        return;
    }

    const QByteArray srcArgs = QByteArray("time_base=1/48000:sample_rate=48000:sample_fmt=s16:channel_layout=stereo");
    if (avfilter_graph_create_filter(&m_abufferCtx, abuf, "in", srcArgs.constData(), nullptr, m_filterGraph) < 0) {
        releaseTempoFilter();
        return;
    }

    AVFilterContext* tempoCtx = nullptr;
    const QByteArray tempoArgs = QByteArray::number(std::clamp(rate, 0.5, 2.0), 'f', 2);
    if (avfilter_graph_create_filter(&tempoCtx, atempo, "tempo", tempoArgs.constData(), nullptr, m_filterGraph) < 0) {
        releaseTempoFilter();
        return;
    }

    if (avfilter_graph_create_filter(&m_abuffersinkCtx, asink, "out", nullptr, nullptr, m_filterGraph) < 0) {
        releaseTempoFilter();
        return;
    }

    if (avfilter_link(m_abufferCtx, 0, tempoCtx, 0) < 0 ||
        avfilter_link(tempoCtx, 0, m_abuffersinkCtx, 0) < 0 ||
        avfilter_graph_config(m_filterGraph, nullptr) < 0) {
        releaseTempoFilter();
        return;
    }
    m_filterRate = rate;
}

void AudioPlayer::pushProcessedS16(int16_t* samples, int frameCount, int srcChannelsForMeter) {
    if (!samples || frameCount <= 0) return;

    // 1. 基于未缩放、未静音的原始解码 PCM 计算真实 RMS & Peak
    double sumSqL = 0.0, sumSqR = 0.0;
    double pkL = 0.0, pkR = 0.0;
    const int chCount = (srcChannelsForMeter >= 2) ? 2 : 1;
    for (int i = 0; i < frameCount; ++i) {
        const double l = static_cast<double>(samples[i * 2]) / 32768.0;
        const double r = static_cast<double>(samples[i * 2 + 1]) / 32768.0;
        const double al = std::abs(l);
        const double ar = std::abs(r);
        sumSqL += l * l;
        sumSqR += r * r;
        if (al > pkL) pkL = al;
        if (ar > pkR) pkR = ar;
    }
    const double rmsL = std::sqrt(sumSqL / static_cast<double>(frameCount));
    const double rmsR = (chCount == 1) ? rmsL : std::sqrt(sumSqR / static_cast<double>(frameCount));
    if (chCount == 1) pkR = pkL;

    {
        QMutexLocker mlk(&m_meterMtx);
        m_rawChannels = chCount;
        m_rawRmsL = rmsL;
        m_rawRmsR = rmsR;
        m_rawPeakL = pkL;
        m_rawPeakR = pkR;
        m_lastFeedEpochMs = TimeUtils::nowEpochMs();
    }

    // 2. 应用 100ms 切换/起播淡入包络，防爆音
    int fadeLeft = m_fadeSamplesLeft.load(std::memory_order_relaxed);
    if (fadeLeft > 0) {
        constexpr int kTotalFade = 4800;
        for (int i = 0; i < frameCount && fadeLeft > 0; ++i, --fadeLeft) {
            const double g = static_cast<double>(kTotalFade - fadeLeft) / static_cast<double>(kTotalFade);
            samples[i * 2] = static_cast<int16_t>(samples[i * 2] * g);
            samples[i * 2 + 1] = static_cast<int16_t>(samples[i * 2 + 1] * g);
        }
        m_fadeSamplesLeft.store(fadeLeft, std::memory_order_relaxed);
    }

    m_device->pushPcm(reinterpret_cast<const uint8_t*>(samples),
                      static_cast<size_t>(frameCount) * 2 * sizeof(int16_t));
}

void AudioPlayer::feedAudioFrame(const AVFrame* frame) {
    if (!m_enabled.load(std::memory_order_acquire) || !frame || frame->nb_samples <= 0) return;

    const double rate = m_rate.load(std::memory_order_relaxed);
    if (rate > 2.2) return;

    QMutexLocker lk(&m_procMtx);
    const int srcCh = frame->ch_layout.nb_channels;
    ensureResampler(frame->sample_rate, srcCh, frame->format, &frame->ch_layout);
    if (!m_swr) return;

    const int maxOutSamples = swr_get_out_samples(m_swr, frame->nb_samples);
    if (maxOutSamples <= 0) return;
    std::vector<int16_t> outBuf(static_cast<size_t>(maxOutSamples) * 2);
    uint8_t* outPtrs[1] = {reinterpret_cast<uint8_t*>(outBuf.data())};
    const int converted = swr_convert(
        m_swr, outPtrs, maxOutSamples,
        const_cast<const uint8_t**>(frame->extended_data), frame->nb_samples);
    if (converted <= 0) return;

    if (std::abs(rate - 1.0) <= 0.05) {
        releaseTempoFilter();
        pushProcessedS16(outBuf.data(), converted, srcCh);
        return;
    }

    ensureTempoFilter(rate);
    if (!m_filterGraph || !m_abufferCtx || !m_abuffersinkCtx) {
        pushProcessedS16(outBuf.data(), converted, srcCh);
        return;
    }

    AVFrame* inF = av_frame_alloc();
    if (!inF) return;
    inF->sample_rate = 48000;
    inF->format = AV_SAMPLE_FMT_S16;
    inF->nb_samples = converted;
    AVChannelLayout stereoLayout = AV_CHANNEL_LAYOUT_STEREO;
    av_channel_layout_copy(&inF->ch_layout, &stereoLayout);
    if (av_frame_get_buffer(inF, 0) >= 0) {
        std::memcpy(inF->data[0], outBuf.data(), static_cast<size_t>(converted) * 2 * sizeof(int16_t));
        if (av_buffersrc_add_frame_flags(m_abufferCtx, inF, AV_BUFFERSRC_FLAG_KEEP_REF) >= 0) {
            AVFrame* filtF = av_frame_alloc();
            if (filtF) {
                while (av_buffersink_get_frame(m_abuffersinkCtx, filtF) >= 0) {
                    if (filtF->nb_samples > 0 && filtF->data[0]) {
                        pushProcessedS16(reinterpret_cast<int16_t*>(filtF->data[0]), filtF->nb_samples, srcCh);
                    }
                    av_frame_unref(filtF);
                }
                av_frame_free(&filtF);
            }
        }
    }
    av_frame_free(&inF);
}

double AudioPlayer::linearToNormDb(double linearAmp, double& outDb) {
    if (linearAmp <= 1e-5) {
        outDb = -60.0;
        return 0.0;
    }
    outDb = std::clamp(20.0 * std::log10(linearAmp), -60.0, 0.0);
    return (outDb + 60.0) / 60.0;
}

AudioMeterState AudioPlayer::sampleMeter(bool allowActive) {
    AudioMeterState s;
    double rawRmsL = 0.0, rawRmsR = 0.0, rawPeakL = 0.0, rawPeakR = 0.0;
    int64_t lastFeed = 0;
    {
        QMutexLocker mlk(&m_meterMtx);
        s.channels = m_rawChannels;
        rawRmsL = m_rawRmsL;
        rawRmsR = m_rawRmsR;
        rawPeakL = m_rawPeakL;
        rawPeakR = m_rawPeakR;
        lastFeed = m_lastFeedEpochMs;
    }

    const int64_t nowMs = TimeUtils::nowEpochMs();
    const bool fresh = allowActive && m_enabled.load(std::memory_order_relaxed) && (nowMs - lastFeed < 350);
    s.signalActive = fresh;

    double targetRmsDbL = -60.0, targetRmsDbR = -60.0;
    const double targetRmsNormL = fresh ? linearToNormDb(rawRmsL, targetRmsDbL) : 0.0;
    const double targetRmsNormR = fresh ? linearToNormDb(rawRmsR, targetRmsDbR) : 0.0;
    double targetPeakDbL = -60.0, targetPeakDbR = -60.0;
    const double targetPeakNormL = fresh ? linearToNormDb(rawPeakL, targetPeakDbL) : 0.0;
    const double targetPeakNormR = fresh ? linearToNormDb(rawPeakR, targetPeakDbR) : 0.0;

    // 快速上升、适度平滑回落
    auto smoothStep = [](double cur, double target) {
        return (target > cur) ? (cur * 0.2 + target * 0.8) : std::max(0.0, cur * 0.78 + target * 0.22 - 0.012);
    };
    m_smoothRmsL = smoothStep(m_smoothRmsL, targetRmsNormL);
    m_smoothRmsR = smoothStep(m_smoothRmsR, targetRmsNormR);

    // Peak Hold 保持 500ms 后平滑回落
    if (targetPeakNormL >= m_holdPeakL) {
        m_holdPeakL = targetPeakNormL;
        m_holdTimeMsL = nowMs;
    } else if (nowMs - m_holdTimeMsL > 500) {
        m_holdPeakL = std::max(m_smoothRmsL, m_holdPeakL - 0.035);
    }

    if (targetPeakNormR >= m_holdPeakR) {
        m_holdPeakR = targetPeakNormR;
        m_holdTimeMsR = nowMs;
    } else if (nowMs - m_holdTimeMsR > 500) {
        m_holdPeakR = std::max(m_smoothRmsR, m_holdPeakR - 0.035);
    }

    s.rmsNormL = m_smoothRmsL;
    s.rmsNormR = (s.channels == 1) ? m_smoothRmsL : m_smoothRmsR;
    s.peakHoldNormL = m_holdPeakL;
    s.peakHoldNormR = (s.channels == 1) ? m_holdPeakL : m_holdPeakR;
    s.rmsDbL = (m_smoothRmsL > 0.001) ? (m_smoothRmsL * 60.0 - 60.0) : -60.0;
    s.rmsDbR = (s.rmsNormR > 0.001) ? (s.rmsNormR * 60.0 - 60.0) : -60.0;
    s.peakDbL = (m_holdPeakL > 0.001) ? (m_holdPeakL * 60.0 - 60.0) : -60.0;
    s.peakDbR = (s.peakHoldNormR > 0.001) ? (s.peakHoldNormR * 60.0 - 60.0) : -60.0;
    return s;
}

} // namespace mcs
