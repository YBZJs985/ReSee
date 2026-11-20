#pragma once

#include "core/AppConfig.h"
#include "core/Types.h"

#include <QHash>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVector>
#include <atomic>
#include <thread>

namespace mcs {

class IndexDatabase;

enum class HealthStatus : int {
    Normal = 0,
    Delayed = 1,
    Offline = 2,
    Unknown = 3
};

inline QString healthStatusLabel(HealthStatus st) {
    switch (st) {
    case HealthStatus::Normal: return QStringLiteral("正常");
    case HealthStatus::Delayed: return QStringLiteral("延迟");
    case HealthStatus::Offline: return QStringLiteral("离线");
    case HealthStatus::Unknown: return QStringLiteral("未知");
    }
    return QStringLiteral("未知");
}

struct ChannelHealthInfo {
    int streamIndex = 0;
    QString channel;       // "00" or "10"
    QString displayName;
    HealthStatus status = HealthStatus::Unknown;
    int64_t lastWriteMs = 0;
    int64_t ageSec = -1;
    QString latestFileName;
    QString latestFilePath;
    QString latestRangeText;
    int64_t lastJumpMs = 0;
};

struct CameraHealthInfo {
    QString cameraId;
    QString displayName;
    QString rootPath;
    bool isDual = false;
    bool folderAccessible = false;
    bool partialAbnormal = false;
    HealthStatus status = HealthStatus::Unknown;
    int64_t lastWriteMs = 0;
    int64_t ageSec = -1;
    QString latestFileName;
    QString latestRangeText;
    int64_t lastJumpMs = 0;
    int64_t totalBytes = 0;
    int64_t fileCount = 0;
    int onlineMaxMin = 30;
    int offlineMaxMin = 60;
    QString thumbPath;
    QString errorMessage;
    QVector<ChannelHealthInfo> channels;
};

struct NasDiskInfo {
    QString rootPath;
    bool ok = false;
    int64_t totalBytes = 0;
    int64_t usedBytes = 0;
    int64_t freeBytes = 0;
    double freeRatio = 0.0;
    int levelCode = 2; // 0=从容(>=40%), 1=偏紧(20%~40%), 2=紧张(<20% 或未知)
    QString levelText = QStringLiteral("未知");
    QString errorMessage;
};

struct HealthSnapshot {
    int64_t checkedAtMs = 0;
    bool nasAccessible = false;
    int normalCount = 0;
    int delayedCount = 0;
    int offlineCount = 0;
    int unknownCount = 0;
    NasDiskInfo disk;
    QVector<CameraHealthInfo> cameras;
};

class HealthMonitor : public QObject {
    Q_OBJECT
public:
    explicit HealthMonitor(IndexDatabase* db, QObject* parent = nullptr);
    ~HealthMonitor() override;

    void configure(const AppConfig& cfg);
    void start();
    void stop();
    void triggerCheckNow();
    void setForegroundBusy(bool busy);

    bool isChecking() const { return m_checking.load(std::memory_order_acquire); }

signals:
    void checkStarted();
    void snapshotUpdated(const mcs::HealthSnapshot& snapshot);
    void newFilesDetectedOnCamera(int streamIndex);

private:
    HealthSnapshot runSingleCheck(const AppConfig& cfg);
    CameraHealthInfo scanCameraHealth(const CameraConfig& cam,
                                      const QVector<StreamConfig>& camStreams,
                                      const QHash<int, int64_t>& streamBytes,
                                      const QHash<int, int64_t>& streamCounts,
                                      int tzOffsetSec,
                                      int64_t nowMs);
    NasDiskInfo readNasDisk(const AppConfig& cfg) const;

    IndexDatabase* m_db = nullptr;
    AppConfig m_cfg;
    QTimer* m_timer = nullptr;

    std::atomic<bool> m_stopRequested{false};
    std::atomic<bool> m_checking{false};
    std::atomic<bool> m_foregroundBusy{false};
    bool m_hasFirstCheck = false;
    QHash<QString, HealthStatus> m_prevStatuses;
    std::jthread m_worker;
};

} // namespace mcs
