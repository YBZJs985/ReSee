#pragma once

#include <QObject>
#include <QTimer>
#include <chrono>
#include <cstdint>

namespace mcs {

class MasterClock : public QObject {
    Q_OBJECT
public:
    explicit MasterClock(QObject* parent = nullptr);

    int64_t currentTimeMs() const;
    bool isPlaying() const { return m_playing; }
    bool isScrubbing() const { return m_scrubbing; }
    double playbackRate() const { return m_rate; }

    void play();
    void pause();
    void togglePlay();
    void setRate(double rate);

    // 精确跳转到指定时间（停止任何正在进行的穿梭防抖，保持或指定播放态）
    void seekExact(int64_t targetMs);

    // 时间轴拖动穿梭接口
    void beginTimelineScrub();
    void updateTimelineScrub(int64_t targetMs);
    void endTimelineScrub();

    // 鼠标普通滚轮时间穿梭：wheelDelta > 0 滚向未来还是过去由调用方传 towardPast
    // 自动计算平滑加速步长，并在停止滚动 220ms 后发出 scrubSettled
    void handleWheelScrub(bool towardPast, int steps, int64_t minTimeMs, int64_t maxTimeMs);

    // 外部直接初始化（如断点续看）
    void setInitialTime(int64_t ms, double rate);

signals:
    void timeChanged(int64_t masterMs);
    void stateChanged(bool playing, double rate, bool scrubbing);
    void scrubPreviewRequested(int64_t targetMs);
    void exactSeekRequested(int64_t targetMs, bool resumePlaying);

private slots:
    void onWheelSettleTimeout();

private:
    void reanchor(int64_t newBaseMs);

    int64_t m_anchorWallMs = 0;
    std::chrono::steady_clock::time_point m_anchorSteady;
    double m_rate = 1.0;
    bool m_playing = false;
    bool m_scrubbing = false;

    // 滚轮与拖动前状态备份，结束后精确还原
    bool m_wasPlayingBeforeScrub = false;
    double m_rateBeforeScrub = 1.0;

    // 滚轮平滑加速状态
    std::chrono::steady_clock::time_point m_lastWheelTime{};
    double m_wheelVelocity = 0.0; // 平滑滚轮能量
    QTimer* m_wheelSettleTimer = nullptr;
};

} // namespace mcs
