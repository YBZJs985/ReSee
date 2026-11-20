#pragma once

#include <QAudioFormat>
#include <QAudioSink>
#include <QIODevice>
#include <QMediaDevices>
#include <QMutex>
#include <QObject>
#include <atomic>
#include <cstdint>
#include <vector>

struct AVFilterContext;
struct AVFilterGraph;
struct AVFrame;
struct SwrContext;

namespace mcs {

struct AudioMeterState {
    int channels = 1;
    double rmsDbL = -60.0;
    double rmsDbR = -60.0;
    double peakDbL = -60.0;
    double peakDbR = -60.0;
    double rmsNormL = 0.0;
    double rmsNormR = 0.0;
    double peakHoldNormL = 0.0;
    double peakHoldNormR = 0.0;
    bool signalActive = false;
};

// 跨平台音频输出环形缓冲与单路独占播放器（Qt Multimedia + FFmpeg atempo）
class AudioRingDevice : public QIODevice {
    Q_OBJECT
public:
    explicit AudioRingDevice(QObject* parent = nullptr);

    void pushPcm(const uint8_t* data, size_t bytes);
    void clear();
    size_t bufferedBytes() const;

protected:
    qint64 readData(char* data, qint64 maxlen) override;
    qint64 writeData(const char* data, qint64 len) override;
    qint64 bytesAvailable() const override;

private:
    mutable QMutex m_mtx;
    std::vector<uint8_t> m_ring;
    size_t m_head = 0;
    size_t m_size = 0;
    static constexpr size_t kMaxRingBytes = 48000 * 2 * 2 * 2; // 2 秒 48kHz Stereo S16
};

class AudioPlayer : public QObject {
    Q_OBJECT
public:
    explicit AudioPlayer(QObject* parent = nullptr);
    ~AudioPlayer() override;

    void setEnabled(bool enabled);
    bool isEnabled() const { return m_enabled.load(std::memory_order_acquire); }

    void setVolume(double vol);
    double volume() const { return m_volume.load(std::memory_order_relaxed); }

    void setMuted(bool muted);
    bool isMuted() const { return m_muted.load(std::memory_order_relaxed); }

    void setPlaybackRate(double rate);
    double playbackRate() const { return m_rate.load(std::memory_order_relaxed); }

    void prepareStreamSwitch();
    void clearBuffer();
    int64_t queuedAudioMs() const;

    // 供有声路解码线程在 0.5x/1x/2x 播放时投喂解码后的 AVFrame
    void feedAudioFrame(const AVFrame* frame);

    // 供 UI 定时器 (~30Hz) 采样平滑后的实时电平与峰值保持状态
    AudioMeterState sampleMeter(bool allowActive);

public slots:
    void startIfNeeded();
    void stopOutput();

private slots:
    void onAudioOutputsChanged();

private:
    void ensureResampler(int srcSampleRate, int srcChannels, int srcFmt, const void* chLayoutPtr);
    void ensureTempoFilter(double rate);
    void releaseTempoFilter();
    void pushProcessedS16(int16_t* samples, int frameCount, int srcChannelsForMeter);
    static double linearToNormDb(double linearAmp, double& outDb);

    std::atomic<bool> m_enabled{false};
    std::atomic<double> m_volume{0.85};
    std::atomic<bool> m_muted{false};
    std::atomic<double> m_rate{1.0};
    std::atomic<int> m_fadeSamplesLeft{0};

    QAudioSink* m_sink = nullptr;
    AudioRingDevice* m_device = nullptr;
    QMediaDevices* m_mediaDevices = nullptr;

    QMutex m_procMtx;
    SwrContext* m_swr = nullptr;
    int m_lastRate = 0;
    int m_lastCh = 0;
    int m_lastFmt = -1;

    AVFilterGraph* m_filterGraph = nullptr;
    AVFilterContext* m_abufferCtx = nullptr;
    AVFilterContext* m_abuffersinkCtx = nullptr;
    double m_filterRate = 1.0;

    // 原始 PCM 瞬时电平（与输出音量/静音完全解耦）
    mutable QMutex m_meterMtx;
    int m_rawChannels = 1;
    double m_rawRmsL = 0.0;
    double m_rawRmsR = 0.0;
    double m_rawPeakL = 0.0;
    double m_rawPeakR = 0.0;
    int64_t m_lastFeedEpochMs = 0;

    // UI 端平滑与 Peak Hold 状态
    double m_smoothRmsL = 0.0;
    double m_smoothRmsR = 0.0;
    double m_holdPeakL = 0.0;
    double m_holdPeakR = 0.0;
    int64_t m_holdTimeMsL = 0;
    int64_t m_holdTimeMsR = 0;
};

} // namespace mcs
