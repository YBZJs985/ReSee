#pragma once

#include <QObject>
#include <QPointF>
#include <QStringList>
#include <QVariantList>
#include <cstdint>

namespace mcs {

class SyncScheduler;

class TimelineModel : public QObject {
    Q_OBJECT
    Q_PROPERTY(qint64 viewStartMs READ viewStartMs NOTIFY rangeChanged)
    Q_PROPERTY(qint64 viewEndMs READ viewEndMs NOTIFY rangeChanged)
    Q_PROPERTY(qint64 totalMinMs READ totalMinMs NOTIFY indexChanged)
    Q_PROPERTY(qint64 totalMaxMs READ totalMaxMs NOTIFY indexChanged)
    Q_PROPERTY(QString viewSpanText READ viewSpanText NOTIFY rangeChanged)
    Q_PROPERTY(QStringList availableDays READ availableDays NOTIFY indexChanged)

public:
    explicit TimelineModel(SyncScheduler* scheduler, int tzOffsetSec, QObject* parent = nullptr);

    qint64 viewStartMs() const { return m_viewStartMs; }
    qint64 viewEndMs() const { return m_viewEndMs; }
    qint64 totalMinMs() const;
    qint64 totalMaxMs() const;
    QString viewSpanText() const;
    QStringList availableDays() const { return m_availableDays; }

    void setTimezoneOffsetSec(int tz) { m_tzOffsetSec = tz; }
    void refreshFromScheduler();

    // 返回 [ { x0: 0..1, x1: 0..1, status: int } ] 供 QML 绘制通道覆盖轨道
    Q_INVOKABLE QVariantList getStreamBars(int streamIndex) const;
    // 返回 各路覆盖合并条
    Q_INVOKABLE QVariantList getCombinedBars() const;
    QList<QPointF> nightBands() const;
    // 获取时间刻度标签 [{ ratio: 0..1, label: "HH:MM(:SS)" }]
    Q_INVOKABLE QVariantList getTicks(int approxCount = 8) const;

    Q_INVOKABLE void zoomAt(double factor, double anchorRatio);
    Q_INVOKABLE void panByRatio(double deltaRatio);
    Q_INVOKABLE void fitFullDay(qint64 anyTimeInDayMs);
    Q_INVOKABLE void fitDateString(const QString& yyyyMmDd);
    Q_INVOKABLE void setSpanSeconds(qint64 spanSec, qint64 centerMs);
    Q_INVOKABLE void ensureTimeVisible(qint64 targetMs);

signals:
    void rangeChanged();
    void indexChanged();

private:
    void clampView();

    SyncScheduler* m_scheduler = nullptr;
    int m_tzOffsetSec = 8 * 3600;
    qint64 m_viewStartMs = 0;
    qint64 m_viewEndMs = 0;
    QStringList m_availableDays;

    static constexpr qint64 kMinSpanMs = 30 * 1000LL;         // 最细缩放至 30 秒窗
    static constexpr qint64 kMaxSpanMs = 7 * 86400 * 1000LL;  // 最宽 7 天窗
};

} // namespace mcs
