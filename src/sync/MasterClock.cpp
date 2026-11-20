#include "sync/MasterClock.h"

#include <algorithm>
#include <cmath>

namespace mcs {

MasterClock::MasterClock(QObject* parent) : QObject(parent) {
    m_anchorSteady = std::chrono::steady_clock::now();
    m_wheelSettleTimer = new QTimer(this);
    m_wheelSettleTimer->setSingleShot(true);
    m_wheelSettleTimer->setInterval(220);
    connect(m_wheelSettleTimer, &QTimer::timeout, this, &MasterClock::onWheelSettleTimeout);
}

int64_t MasterClock::currentTimeMs() const {
    if (!m_playing || m_scrubbing) {
        return m_anchorWallMs;
    }
    const auto now = std::chrono::steady_clock::now();
    const double elapsedMs = std::chrono::duration<double, std::milli>(now - m_anchorSteady).count();
    return m_anchorWallMs + static_cast<int64_t>(std::llround(elapsedMs * m_rate));
}

void MasterClock::reanchor(int64_t newBaseMs) {
    m_anchorWallMs = newBaseMs;
    m_anchorSteady = std::chrono::steady_clock::now();
}

void MasterClock::setInitialTime(int64_t ms, double rate) {
    m_playing = false;
    m_scrubbing = false;
    m_rate = std::clamp(rate, 0.25, 16.0);
    reanchor(ms);
    emit timeChanged(ms);
    emit stateChanged(m_playing, m_rate, m_scrubbing);
}

void MasterClock::play() {
    if (m_scrubbing) {
        m_wheelSettleTimer->stop();
        m_scrubbing = false;
    }
    if (m_playing) return;
    reanchor(m_anchorWallMs);
    m_playing = true;
    emit stateChanged(m_playing, m_rate, m_scrubbing);
}

void MasterClock::pause() {
    if (!m_playing) return;
    const int64_t cur = currentTimeMs();
    m_playing = false;
    reanchor(cur);
    emit timeChanged(cur);
    emit stateChanged(m_playing, m_rate, m_scrubbing);
}

void MasterClock::togglePlay() {
    if (m_playing) pause();
    else play();
}

void MasterClock::setRate(double rate) {
    const double clamped = std::clamp(rate, 0.25, 16.0);
    if (std::abs(clamped - m_rate) < 0.001) return;
    const int64_t cur = currentTimeMs();
    m_rate = clamped;
    reanchor(cur);
    emit stateChanged(m_playing, m_rate, m_scrubbing);
}

void MasterClock::seekExact(int64_t targetMs) {
    m_wheelSettleTimer->stop();
    m_scrubbing = false;
    reanchor(targetMs);
    emit timeChanged(targetMs);
    emit stateChanged(m_playing, m_rate, m_scrubbing);
    emit exactSeekRequested(targetMs, m_playing);
}

void MasterClock::beginTimelineScrub() {
    if (m_scrubbing) return;
    m_wheelSettleTimer->stop();
    m_wasPlayingBeforeScrub = m_playing;
    m_rateBeforeScrub = m_rate;
    const int64_t cur = currentTimeMs();
    m_playing = false;
    m_scrubbing = true;
    reanchor(cur);
    emit stateChanged(m_playing, m_rate, m_scrubbing);
}

void MasterClock::updateTimelineScrub(int64_t targetMs) {
    if (!m_scrubbing) {
        beginTimelineScrub();
    }
    reanchor(targetMs);
    emit timeChanged(targetMs);
    emit scrubPreviewRequested(targetMs);
}

void MasterClock::endTimelineScrub() {
    if (!m_scrubbing) return;
    m_scrubbing = false;
    m_rate = m_rateBeforeScrub;
    m_playing = m_wasPlayingBeforeScrub;
    reanchor(m_anchorWallMs);
    emit stateChanged(m_playing, m_rate, m_scrubbing);
    emit exactSeekRequested(m_anchorWallMs, m_playing);
}

void MasterClock::handleWheelScrub(bool towardPast, int steps, int64_t minTimeMs, int64_t maxTimeMs) {
    const auto now = std::chrono::steady_clock::now();
    if (!m_scrubbing) {
        m_wasPlayingBeforeScrub = m_playing;
        m_rateBeforeScrub = m_rate;
        const int64_t cur = currentTimeMs();
        m_playing = false;
        m_scrubbing = true;
        m_wheelVelocity = 0.0;
        reanchor(cur);
        emit stateChanged(m_playing, m_rate, m_scrubbing);
    }

    // 计算滚轮间隔并生成平滑加速步长曲线：
    // 慢速单格(>280ms): ~1s; 中速(80~280ms): 2s~12s; 快速连续(<80ms): 平滑提升至 20s~90s
    double dtMs = 500.0;
    if (m_lastWheelTime.time_since_epoch().count() > 0) {
        dtMs = std::chrono::duration<double, std::milli>(now - m_lastWheelTime).count();
    }
    m_lastWheelTime = now;

    if (dtMs > 320.0) {
        m_wheelVelocity = 0.0;
    } else {
        // 指数衰减旧能量 + 叠加新操作密度
        const double decay = std::exp(-dtMs / 240.0);
        const double impulse = std::clamp(160.0 / std::max(dtMs, 18.0), 0.3, 6.0);
        m_wheelVelocity = std::clamp(m_wheelVelocity * decay + impulse, 0.0, 28.0);
    }

    // 平滑幂函数曲线映射步长 (ms)
    const double stepSec = 1.0 + 0.32 * std::pow(m_wheelVelocity, 1.65);
    const int64_t deltaMs = static_cast<int64_t>(std::llround(stepSec * 1000.0)) * std::max(1, steps);

    int64_t nextMs = m_anchorWallMs + (towardPast ? -deltaMs : deltaMs);
    if (maxTimeMs > minTimeMs) {
        nextMs = std::clamp(nextMs, minTimeMs, maxTimeMs);
    }
    reanchor(nextMs);
    emit timeChanged(nextMs);
    emit scrubPreviewRequested(nextMs);

    // 重置防抖计时器：用户停止滚动 220ms 后精确 Seek 并恢复之前状态
    m_wheelSettleTimer->start();
}

void MasterClock::onWheelSettleTimeout() {
    if (!m_scrubbing) return;
    m_scrubbing = false;
    m_wheelVelocity = 0.0;
    m_rate = m_rateBeforeScrub;
    m_playing = m_wasPlayingBeforeScrub;
    reanchor(m_anchorWallMs);
    emit stateChanged(m_playing, m_rate, m_scrubbing);
    emit exactSeekRequested(m_anchorWallMs, m_playing);
}

} // namespace mcs
