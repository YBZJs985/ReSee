#include "app/BookmarkModel.h"
#include "core/TimeUtils.h"
#include "index/IndexDatabase.h"

#include <QFile>
#include <QUrl>
#include <QVariantMap>

namespace mcs {

BookmarkModel::BookmarkModel(IndexDatabase* db, int tzOffsetSec, QObject* parent)
    : QAbstractListModel(parent), m_db(db), m_tzOffsetSec(tzOffsetSec) {}

int BookmarkModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : m_items.size();
}

QVariant BookmarkModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_items.size()) return {};
    const auto& b = m_items.at(index.row());
    switch (role) {
    case IdRole: return static_cast<qint64>(b.id);
    case TimestampMsRole: return static_cast<qint64>(b.timestampMs);
    case TimeTextRole: return TimeUtils::formatEpochMs(b.timestampMs, m_tzOffsetSec);
    case TitleRole: return b.title;
    case NoteRole: return b.note;
    case ColorRole: return b.color;
    case StreamIndexRole: return b.streamIndex;
    case ScreenshotPathRole: return b.screenshotPath.isEmpty() ? QString() : QUrl::fromLocalFile(b.screenshotPath).toString();
    }
    return {};
}

QHash<int, QByteArray> BookmarkModel::roleNames() const {
    return {
        {IdRole, "bookmarkId"},
        {TimestampMsRole, "timestampMs"},
        {TimeTextRole, "timeText"},
        {TitleRole, "title"},
        {NoteRole, "note"},
        {ColorRole, "color"},
        {StreamIndexRole, "streamIndex"},
        {ScreenshotPathRole, "screenshotUrl"}
    };
}

void BookmarkModel::setSearchQuery(const QString& q) {
    if (m_searchQuery == q) return;
    m_searchQuery = q;
    emit filterChanged();
    reload();
}

void BookmarkModel::setDateFilter(const QString& d) {
    if (m_dateFilter == d) return;
    m_dateFilter = d;
    emit filterChanged();
    reload();
}

void BookmarkModel::reload() {
    if (!m_db) return;
    int64_t fromMs = 0;
    int64_t toMs = 0;
    if (!m_dateFilter.trimmed().isEmpty()) {
        if (auto ds = TimeUtils::parseDateTimeString(m_dateFilter.trimmed() + QStringLiteral(" 00:00:00"), m_tzOffsetSec)) {
            fromMs = *ds;
            toMs = *ds + 86400LL * 1000LL - 1;
        }
    }

    beginResetModel();
    m_items = m_db->listBookmarks(m_searchQuery, fromMs, toMs);
    m_allItems = m_db->listBookmarks();
    endResetModel();
    emit countChanged();
}

qint64 BookmarkModel::addBookmark(qint64 timestampMs, const QString& title, const QString& note,
                                  const QString& color, int streamIndex, const QString& screenshotPath) {
    if (!m_db) return 0;
    BookmarkItem item;
    item.timestampMs = timestampMs;
    item.title = title.trimmed().isEmpty() ? QStringLiteral("时间标记") : title.trimmed();
    item.note = note.trimmed();
    item.color = color.isEmpty() ? QStringLiteral("#3B82F6") : color;
    item.streamIndex = streamIndex;
    item.screenshotPath = screenshotPath;
    item.createdAtMs = TimeUtils::nowEpochMs();
    const int64_t id = m_db->addBookmark(item);
    reload();
    return id;
}

bool BookmarkModel::updateBookmark(qint64 id, const QString& title, const QString& note,
                                   const QString& color, int streamIndex) {
    if (!m_db) return false;
    for (const auto& b : m_allItems) {
        if (b.id == id) {
            BookmarkItem updated = b;
            updated.title = title.trimmed();
            updated.note = note.trimmed();
            updated.color = color;
            updated.streamIndex = streamIndex;
            const bool ok = m_db->updateBookmark(updated);
            reload();
            return ok;
        }
    }
    return false;
}

bool BookmarkModel::deleteBookmark(qint64 id) {
    if (!m_db) return false;
    QString shotPath;
    for (const auto& b : m_allItems) {
        if (b.id == id) {
            shotPath = b.screenshotPath;
            break;
        }
    }
    const bool ok = m_db->deleteBookmark(id);
    if (ok && !shotPath.isEmpty()) {
        QFile::remove(shotPath);
    }
    reload();
    return ok;
}

QVariantList BookmarkModel::bookmarksInRange(qint64 startMs, qint64 endMs) const {
    QVariantList out;
    if (endMs <= startMs) return out;
    const double span = static_cast<double>(endMs - startMs);
    for (const auto& b : m_allItems) {
        if (b.timestampMs >= startMs && b.timestampMs <= endMs) {
            QVariantMap m;
            m.insert(QStringLiteral("bookmarkId"), static_cast<qint64>(b.id));
            m.insert(QStringLiteral("timestampMs"), static_cast<qint64>(b.timestampMs));
            m.insert(QStringLiteral("ratio"), static_cast<double>(b.timestampMs - startMs) / span);
            m.insert(QStringLiteral("title"), b.title);
            m.insert(QStringLiteral("color"), b.color);
            out.append(m);
        }
    }
    return out;
}

} // namespace mcs
