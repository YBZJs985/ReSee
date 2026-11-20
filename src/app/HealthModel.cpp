#include "app/HealthModel.h"
#include "core/TimeUtils.h"

#include <QMetaType>
#include <QVariantMap>
#include <algorithm>

namespace mcs {

namespace {

QString formatSize(int64_t bytes) {
    if (bytes <= 0) return QStringLiteral("0 B");
    const double kb = 1024.0;
    double v = static_cast<double>(bytes);
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int u = 0;
    while (v >= kb && u < 4) {
        v /= kb;
        ++u;
    }
    return QStringLiteral("%1 %2").arg(v, 0, 'f', u == 0 ? 0 : (u >= 3 ? 2 : 1)).arg(QLatin1String(units[u]));
}

QString formatAge(int64_t ageSec) {
    if (ageSec < 0) return QStringLiteral("—");
    if (ageSec < 60) return QStringLiteral("刚刚");
    const int64_t mins = ageSec / 60;
    if (mins < 60) return QStringLiteral("%1 分钟前").arg(mins);
    const double hours = static_cast<double>(ageSec) / 3600.0;
    if (hours < 24.0) return QStringLiteral("%1 小时前").arg(hours, 0, 'f', 1);
    return QStringLiteral("%1 天前").arg(hours / 24.0, 0, 'f', 1);
}

} // namespace

HealthModel::HealthModel(HealthMonitor* monitor, int tzOffsetSec, QObject* parent)
    : QAbstractListModel(parent), m_monitor(monitor), m_tzOffsetSec(tzOffsetSec) {
    qRegisterMetaType<mcs::HealthSnapshot>("mcs::HealthSnapshot");
    if (m_monitor) {
        connect(m_monitor, &HealthMonitor::checkStarted, this, &HealthModel::checkingChanged);
        connect(m_monitor, &HealthMonitor::snapshotUpdated, this, &HealthModel::onSnapshotUpdated, Qt::QueuedConnection);
    }
}

int HealthModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : m_snapshot.cameras.size();
}

QVariant HealthModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_snapshot.cameras.size()) return {};
    const auto& c = m_snapshot.cameras.at(index.row());
    switch (role) {
    case CameraIdRole: return c.cameraId;
    case DisplayNameRole: return c.displayName;
    case StatusCodeRole: return static_cast<int>(c.status);
    case StatusTextRole: {
        QString txt = healthStatusLabel(c.status);
        if (c.partialAbnormal) txt += QStringLiteral("(部分异常)");
        return txt;
    }
    case PartialAbnormalRole: return c.partialAbnormal;
    case LastWriteTextRole:
        return c.lastWriteMs > 0 ? TimeUtils::formatEpochMs(c.lastWriteMs, m_tzOffsetSec) : QStringLiteral("—");
    case AgeTextRole: return formatAge(c.ageSec);
    case LatestFileNameRole: return c.latestFileName.isEmpty() ? QStringLiteral("—") : c.latestFileName;
    case LatestRangeTextRole: return c.latestRangeText.isEmpty() ? QStringLiteral("—") : c.latestRangeText;
    case LastJumpMsRole: return static_cast<qint64>(c.lastJumpMs);
    case StorageTextRole: return formatSize(c.totalBytes);
    case StorageRatioRole:
        return (m_snapshot.disk.ok && m_snapshot.disk.totalBytes > 0)
            ? std::clamp(static_cast<double>(c.totalBytes) / static_cast<double>(m_snapshot.disk.totalBytes), 0.0, 1.0)
            : 0.0;
    case FileCountRole: return static_cast<qint64>(c.fileCount);
    case IsDualRole: return c.isDual;
    case ChannelsRole: {
        QVariantList list;
        for (const auto& ch : c.channels) {
            QVariantMap m;
            m.insert(QStringLiteral("streamIndex"), ch.streamIndex);
            m.insert(QStringLiteral("channel"), ch.channel);
            m.insert(QStringLiteral("displayName"), ch.displayName);
            m.insert(QStringLiteral("statusCode"), static_cast<int>(ch.status));
            m.insert(QStringLiteral("statusText"), healthStatusLabel(ch.status));
            m.insert(QStringLiteral("lastWriteText"),
                     ch.lastWriteMs > 0 ? TimeUtils::formatEpochMs(ch.lastWriteMs, m_tzOffsetSec) : QStringLiteral("—"));
            m.insert(QStringLiteral("ageText"), formatAge(ch.ageSec));
            m.insert(QStringLiteral("latestFileName"), ch.latestFileName.isEmpty() ? QStringLiteral("—") : ch.latestFileName);
            m.insert(QStringLiteral("latestRangeText"), ch.latestRangeText.isEmpty() ? QStringLiteral("—") : ch.latestRangeText);
            m.insert(QStringLiteral("lastJumpMs"), static_cast<qint64>(ch.lastJumpMs));
            list.append(m);
        }
        return list;
    }
    case ErrorMessageRole: return c.errorMessage;
    case OnlineMaxMinRole: return c.onlineMaxMin;
    case OfflineMaxMinRole: return c.offlineMaxMin;
    case ThumbUrlRole:
        return c.thumbPath.isEmpty() ? QString() : (QStringLiteral("file:///") + c.thumbPath);
    }
    return {};
}

QHash<int, QByteArray> HealthModel::roleNames() const {
    return {
        {CameraIdRole, "cameraId"},
        {DisplayNameRole, "displayName"},
        {StatusCodeRole, "statusCode"},
        {StatusTextRole, "statusText"},
        {PartialAbnormalRole, "partialAbnormal"},
        {LastWriteTextRole, "lastWriteText"},
        {AgeTextRole, "ageText"},
        {LatestFileNameRole, "latestFileName"},
        {LatestRangeTextRole, "latestRangeText"},
        {LastJumpMsRole, "lastJumpMs"},
        {StorageTextRole, "storageText"},
        {StorageRatioRole, "storageRatio"},
        {FileCountRole, "fileCount"},
        {IsDualRole, "isDual"},
        {ChannelsRole, "channels"},
        {ErrorMessageRole, "errorMessage"},
        {OnlineMaxMinRole, "onlineMaxMin"},
        {OfflineMaxMinRole, "offlineMaxMin"},
        {ThumbUrlRole, "thumbUrl"}
    };
}

void HealthModel::checkNow() {
    if (m_monitor) {
        m_monitor->triggerCheckNow();
    }
}

bool HealthModel::checking() const {
    return m_monitor && m_monitor->isChecking();
}

void HealthModel::onSnapshotUpdated(const mcs::HealthSnapshot& snap) {
    beginResetModel();
    m_snapshot = snap;
    endResetModel();
    emit checkingChanged();
    emit summaryChanged();
}

QString HealthModel::summaryText() const {
    if (m_snapshot.cameras.isEmpty()) {
        return checking() ? QStringLiteral("正在检测录像状态...") : QStringLiteral("等待检测");
    }
    if (!m_snapshot.nasAccessible) {
        return QStringLiteral("NAS 不可访问 · 全部未知");
    }
    QStringList parts;
    parts.append(QStringLiteral("%1/%2 正常").arg(m_snapshot.normalCount).arg(m_snapshot.cameras.size()));
    if (m_snapshot.delayedCount > 0) parts.append(QStringLiteral("%1 延迟").arg(m_snapshot.delayedCount));
    if (m_snapshot.offlineCount > 0) parts.append(QStringLiteral("%1 离线").arg(m_snapshot.offlineCount));
    if (m_snapshot.unknownCount > 0) parts.append(QStringLiteral("%1 未知").arg(m_snapshot.unknownCount));
    return parts.join(QStringLiteral(" · "));
}

QString HealthModel::lastCheckText() const {
    if (m_snapshot.checkedAtMs <= 0) return QStringLiteral("尚未检测");
    return TimeUtils::formatTimeOnlyMs(m_snapshot.checkedAtMs, m_tzOffsetSec);
}

QString HealthModel::diskTotalText() const {
    return m_snapshot.disk.ok ? formatSize(m_snapshot.disk.totalBytes) : QStringLiteral("—");
}

QString HealthModel::diskUsedText() const {
    return m_snapshot.disk.ok ? formatSize(m_snapshot.disk.usedBytes) : QStringLiteral("—");
}

QString HealthModel::diskFreeText() const {
    return m_snapshot.disk.ok ? formatSize(m_snapshot.disk.freeBytes) : QStringLiteral("未知");
}

double HealthModel::diskUsedRatio() const {
    if (!m_snapshot.disk.ok || m_snapshot.disk.totalBytes <= 0) return 0.0;
    return std::clamp(static_cast<double>(m_snapshot.disk.usedBytes) / static_cast<double>(m_snapshot.disk.totalBytes), 0.0, 1.0);
}

QString HealthModel::diskFreePctText() const {
    if (!m_snapshot.disk.ok || m_snapshot.disk.totalBytes <= 0) return QStringLiteral("未知");
    return QStringLiteral("%1%").arg(m_snapshot.disk.freeRatio * 100.0, 0, 'f', 1);
}

} // namespace mcs
