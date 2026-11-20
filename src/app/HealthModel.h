#pragma once

#include "health/HealthMonitor.h"

#include <QAbstractListModel>
#include <QVariantList>

namespace mcs {

class HealthModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY summaryChanged)
    Q_PROPERTY(bool checking READ checking NOTIFY checkingChanged)
    Q_PROPERTY(bool nasAccessible READ nasAccessible NOTIFY summaryChanged)
    Q_PROPERTY(int normalCount READ normalCount NOTIFY summaryChanged)
    Q_PROPERTY(int delayedCount READ delayedCount NOTIFY summaryChanged)
    Q_PROPERTY(int offlineCount READ offlineCount NOTIFY summaryChanged)
    Q_PROPERTY(int unknownCount READ unknownCount NOTIFY summaryChanged)
    Q_PROPERTY(QString summaryText READ summaryText NOTIFY summaryChanged)
    Q_PROPERTY(QString lastCheckText READ lastCheckText NOTIFY summaryChanged)

    Q_PROPERTY(bool diskOk READ diskOk NOTIFY summaryChanged)
    Q_PROPERTY(QString diskTotalText READ diskTotalText NOTIFY summaryChanged)
    Q_PROPERTY(QString diskUsedText READ diskUsedText NOTIFY summaryChanged)
    Q_PROPERTY(QString diskFreeText READ diskFreeText NOTIFY summaryChanged)
    Q_PROPERTY(double diskUsedRatio READ diskUsedRatio NOTIFY summaryChanged)
    Q_PROPERTY(QString diskFreePctText READ diskFreePctText NOTIFY summaryChanged)
    Q_PROPERTY(int diskLevelCode READ diskLevelCode NOTIFY summaryChanged)
    Q_PROPERTY(QString diskLevelText READ diskLevelText NOTIFY summaryChanged)

public:
    enum Roles {
        CameraIdRole = Qt::UserRole + 1,
        DisplayNameRole,
        StatusCodeRole,
        StatusTextRole,
        PartialAbnormalRole,
        LastWriteTextRole,
        AgeTextRole,
        LatestFileNameRole,
        LatestRangeTextRole,
        LastJumpMsRole,
        StorageTextRole,
        StorageRatioRole,
        FileCountRole,
        IsDualRole,
        ChannelsRole,
        ErrorMessageRole,
        OnlineMaxMinRole,
        OfflineMaxMinRole,
        ThumbUrlRole
    };

    explicit HealthModel(HealthMonitor* monitor, int tzOffsetSec, QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setTimezoneOffsetSec(int tz) { m_tzOffsetSec = tz; }
    Q_INVOKABLE void checkNow();

    bool checking() const;
    bool nasAccessible() const { return m_snapshot.nasAccessible; }
    int normalCount() const { return m_snapshot.normalCount; }
    int delayedCount() const { return m_snapshot.delayedCount; }
    int offlineCount() const { return m_snapshot.offlineCount; }
    int unknownCount() const { return m_snapshot.unknownCount; }
    QString summaryText() const;
    QString lastCheckText() const;

    bool diskOk() const { return m_snapshot.disk.ok; }
    QString diskTotalText() const;
    QString diskUsedText() const;
    QString diskFreeText() const;
    double diskUsedRatio() const;
    QString diskFreePctText() const;
    int diskLevelCode() const { return m_snapshot.disk.levelCode; }
    QString diskLevelText() const { return m_snapshot.disk.levelText; }

public slots:
    void onSnapshotUpdated(const mcs::HealthSnapshot& snap);

signals:
    void checkingChanged();
    void summaryChanged();

private:
    HealthMonitor* m_monitor = nullptr;
    int m_tzOffsetSec = 8 * 3600;
    HealthSnapshot m_snapshot;
};

} // namespace mcs
