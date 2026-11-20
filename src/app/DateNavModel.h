#pragma once

#include <QAbstractListModel>
#include <QVariantList>
#include <QVector>
#include <cstdint>

namespace mcs {

class SyncScheduler;

struct DateNavEntry {
    QString dateStr;      // "2026-11-17"
    QString monthDayText; // "11.17"
    QString yearText;     // "2026"
    QString weekdayText;  // "周二"
    qint64 dayStartMs = 0;
    bool hasVideo = false;
    bool isToday = false;
    int maxStreams = 0;
    double sunriseFrac = 0.25;
    double sunsetFrac = 0.75;
    QVariantList coverageSlots; // 96 个 15 分钟槽位，每槽记录该时段有录像的通道数
};

class DateNavModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY listChanged)
    Q_PROPERTY(bool onlyWithVideo READ onlyWithVideo WRITE setOnlyWithVideo NOTIFY filterChanged)
    Q_PROPERTY(bool recent7DaysOnly READ recent7DaysOnly WRITE setRecent7DaysOnly NOTIFY filterChanged)
    Q_PROPERTY(QString selectedDateStr READ selectedDateStr NOTIFY selectionChanged)
    Q_PROPERTY(int selectedRowIndex READ selectedRowIndex NOTIFY selectionChanged)
    Q_PROPERTY(double currentDayFraction READ currentDayFraction NOTIFY selectionChanged)

public:
    enum Roles {
        DateStrRole = Qt::UserRole + 1,
        MonthDayRole,
        YearRole,
        WeekdayRole,
        DayStartMsRole,
        HasVideoRole,
        IsTodayRole,
        MaxStreamsRole,
        SunriseFracRole,
        SunsetFracRole,
        CoverageSlotsRole,
        IsSelectedRole
    };

    explicit DateNavModel(SyncScheduler* scheduler, int tzOffsetSec, QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setTimezoneOffsetSec(int tz) { m_tzOffsetSec = tz; }
    void refreshFromScheduler();
    void updateMasterTime(int64_t masterWallMs);

    bool onlyWithVideo() const { return m_onlyWithVideo; }
    void setOnlyWithVideo(bool v);

    bool recent7DaysOnly() const { return m_recent7DaysOnly; }
    void setRecent7DaysOnly(bool v);

    QString selectedDateStr() const { return m_selectedDateStr; }
    int selectedRowIndex() const { return m_selectedRowIndex; }
    double currentDayFraction() const { return m_currentDayFraction; }

    Q_INVOKABLE QString hoverInfoForSlot(int rowIndex, double fraction) const;
    Q_INVOKABLE double coverage(const QString& date) const;
    Q_INVOKABLE bool hasGap(const QString& date) const;
    Q_INVOKABLE QString todayDateStr() const;
    Q_INVOKABLE QString yesterdayDateStr() const;

signals:
    void listChanged();
    void filterChanged();
    void selectionChanged();

private:
    void rebuildFilteredRows();
    void updateSelectionRow();

    SyncScheduler* m_scheduler = nullptr;
    int m_tzOffsetSec = 8 * 3600;
    bool m_onlyWithVideo = true;
    bool m_recent7DaysOnly = false;

    QString m_selectedDateStr;
    int m_selectedRowIndex = -1;
    double m_currentDayFraction = 0.0;

    QVector<DateNavEntry> m_allEntries;
    QVector<DateNavEntry> m_rows;
};

} // namespace mcs
