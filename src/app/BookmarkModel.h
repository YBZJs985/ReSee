#pragma once

#include "core/Types.h"

#include <QAbstractListModel>
#include <QVector>

namespace mcs {

class IndexDatabase;

class BookmarkModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QString searchQuery READ searchQuery WRITE setSearchQuery NOTIFY filterChanged)
    Q_PROPERTY(QString dateFilter READ dateFilter WRITE setDateFilter NOTIFY filterChanged)
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)

public:
    enum Roles {
        IdRole = Qt::UserRole + 1,
        TimestampMsRole,
        TimeTextRole,
        TitleRole,
        NoteRole,
        ColorRole,
        StreamIndexRole,
        ScreenshotPathRole
    };

    explicit BookmarkModel(IndexDatabase* db, int tzOffsetSec, QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    QString searchQuery() const { return m_searchQuery; }
    void setSearchQuery(const QString& q);

    QString dateFilter() const { return m_dateFilter; }
    void setDateFilter(const QString& d);

    void setTimezoneOffsetSec(int tz) { m_tzOffsetSec = tz; }
    Q_INVOKABLE void reload();
    Q_INVOKABLE qint64 addBookmark(qint64 timestampMs, const QString& title, const QString& note,
                                   const QString& color, int streamIndex, const QString& screenshotPath);
    Q_INVOKABLE bool updateBookmark(qint64 id, const QString& title, const QString& note,
                                    const QString& color, int streamIndex);
    Q_INVOKABLE bool deleteBookmark(qint64 id);

    // 供时间轴绘制当前可视窗口内的标记点
    Q_INVOKABLE QVariantList bookmarksInRange(qint64 startMs, qint64 endMs) const;

signals:
    void filterChanged();
    void countChanged();

private:
    IndexDatabase* m_db = nullptr;
    int m_tzOffsetSec = 8 * 3600;
    QString m_searchQuery;
    QString m_dateFilter;
    QVector<BookmarkItem> m_items;
    QVector<BookmarkItem> m_allItems;
};

} // namespace mcs
