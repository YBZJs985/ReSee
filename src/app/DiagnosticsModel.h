#pragma once

#include "core/Types.h"

#include <QAbstractListModel>
#include <QVector>

namespace mcs {

class SyncScheduler;
class NasBlockCache;

class DiagnosticsModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(double memCacheMb READ memCacheMb NOTIFY summaryChanged)
    Q_PROPERTY(double memCacheMaxMb READ memCacheMaxMb NOTIFY summaryChanged)
    Q_PROPERTY(double ssdCacheGb READ ssdCacheGb NOTIFY summaryChanged)
    Q_PROPERTY(double ssdCacheMaxGb READ ssdCacheMaxGb NOTIFY summaryChanged)
    Q_PROPERTY(double cacheHitRate READ cacheHitRate NOTIFY summaryChanged)
    Q_PROPERTY(double avgNasReadMs READ avgNasReadMs NOTIFY summaryChanged)
    Q_PROPERTY(bool nasDisconnected READ nasDisconnected NOTIFY summaryChanged)
    Q_PROPERTY(double processMemMb READ processMemMb NOTIFY summaryChanged)
    Q_PROPERTY(double cpuUsagePct READ cpuUsagePct NOTIFY summaryChanged)
    Q_PROPERTY(qint64 maxSyncErrorMs READ maxSyncErrorMs NOTIFY summaryChanged)
    Q_PROPERTY(bool hasValidSyncError READ hasValidSyncError NOTIFY summaryChanged)
    Q_PROPERTY(QString globalStateText READ globalStateText NOTIFY summaryChanged)
    Q_PROPERTY(int globalStateCode READ globalStateCode NOTIFY summaryChanged)
    Q_PROPERTY(int syncedCount READ syncedCount NOTIFY summaryChanged)
    Q_PROPERTY(int noVideoCount READ noVideoCount NOTIFY summaryChanged)
    Q_PROPERTY(int notReadyCount READ notReadyCount NOTIFY summaryChanged)
    Q_PROPERTY(QStringList recentLogs READ recentLogs NOTIFY summaryChanged)

public:
    enum Roles {
        StreamIndexRole = Qt::UserRole + 1,
        DisplayNameRole,
        StateTextRole,
        StateCodeRole,
        WallTimeTextRole,
        SyncErrorMsRole,
        SyncErrorTextRole,
        HasValidTimeRole,
        WaitMsRole,
        FpsRole,
        QueueDepthRole,
        HwBackendRole,
        OffsetMsRole,
        CalibConfidenceRole,
        SeekMsRole,
        NasReadMsRole,
        ResolutionRole,
        CurrentFileRole,
        ErrorMsgRole
    };

    explicit DiagnosticsModel(SyncScheduler* scheduler, NasBlockCache* cache, int tzOffsetSec, QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setTimezoneOffsetSec(int tz) { m_tzOffsetSec = tz; }
    void refresh();

    double memCacheMb() const { return m_memCacheMb; }
    double memCacheMaxMb() const { return m_memCacheMaxMb; }
    double ssdCacheGb() const { return m_ssdCacheGb; }
    double ssdCacheMaxGb() const { return m_ssdCacheMaxGb; }
    double cacheHitRate() const { return m_cacheHitRate; }
    double avgNasReadMs() const { return m_avgNasReadMs; }
    bool nasDisconnected() const { return m_nasDisconnected; }
    double processMemMb() const { return m_processMemMb; }
    double cpuUsagePct() const { return m_cpuUsagePct; }
    qint64 maxSyncErrorMs() const { return m_maxSyncErrorMs; }
    bool hasValidSyncError() const { return m_hasValidSyncError; }
    QString globalStateText() const { return m_globalStateText; }
    int globalStateCode() const { return m_globalStateCode; }
    int syncedCount() const { return m_syncedCount; }
    int noVideoCount() const { return m_noVideoCount; }
    int notReadyCount() const { return m_notReadyCount; }
    QStringList recentLogs() const;

signals:
    void summaryChanged();

private:
    void sampleSystemUsage();

    SyncScheduler* m_scheduler = nullptr;
    NasBlockCache* m_cache = nullptr;
    int m_tzOffsetSec = 8 * 3600;
    QVector<StreamDiagnostics> m_rows;

    double m_memCacheMb = 0.0;
    double m_memCacheMaxMb = 512.0;
    double m_ssdCacheGb = 0.0;
    double m_ssdCacheMaxGb = 15.0;
    double m_cacheHitRate = 0.0;
    double m_avgNasReadMs = 0.0;
    bool m_nasDisconnected = false;
    double m_processMemMb = 0.0;
    double m_cpuUsagePct = 0.0;
    qint64 m_maxSyncErrorMs = 0;
    bool m_hasValidSyncError = false;
    QString m_globalStateText = QStringLiteral("空闲");
    int m_globalStateCode = 1;
    int m_syncedCount = 0;
    int m_noVideoCount = 0;
    int m_notReadyCount = 0;

    uint64_t m_prevCpuKernel = 0;
    uint64_t m_prevCpuUser = 0;
    uint64_t m_prevWallTime = 0;
};

} // namespace mcs
