#include "render/TimelineTrackItem.h"

#include <QPainter>
#include <algorithm>

namespace mcs {

TimelineTrackItem::TimelineTrackItem(QQuickItem* parent) : QQuickPaintedItem(parent) {
    connect(this, &TimelineTrackItem::colorsChanged, this, [this] { update(); });
}

void TimelineTrackItem::setModel(QObject* model) {
    auto* timeline = qobject_cast<TimelineModel*>(model);
    if (m_model == timeline) return;
    if (m_model) disconnect(m_model, nullptr, this, nullptr);
    m_model = timeline;
    if (m_model) {
        connect(m_model, &TimelineModel::rangeChanged, this, &QQuickItem::polish);
        connect(m_model, &TimelineModel::indexChanged, this, &QQuickItem::polish);
        connect(m_model, &QObject::destroyed, this, &QQuickItem::polish);
    }
    polish();
    emit modelChanged();
}

void TimelineTrackItem::setStream(int stream) {
    if (m_stream == stream) return;
    m_stream = stream;
    polish();
    emit streamChanged();
}

void TimelineTrackItem::updatePolish() {
    m_bars = !m_model ? QVariantList{} :
        m_stream < 0 ? m_model->getCombinedBars() : m_model->getStreamBars(m_stream);
    m_nightBands = m_model ? m_model->nightBands() : QList<QPointF>{};
    update();
}

void TimelineTrackItem::paint(QPainter* painter) {
    auto nightBackground = m_nightColor;
    nightBackground.setAlphaF(0.12);
    for (const auto& band : m_nightBands) {
        painter->fillRect(QRectF(band.x() * width(), 0, (band.y() - band.x()) * width(), height()),
                          nightBackground);
    }
    for (const auto& entry : m_bars) {
        const auto bar = entry.toMap();
        const qreal left = bar.value(QStringLiteral("x0")).toDouble() * width();
        const qreal right = bar.value(QStringLiteral("x1")).toDouble() * width();
        const auto status = bar.value(QStringLiteral("status")).toInt();
        const auto& color = status == 2 ? m_errorColor : status == 1 ? m_writingColor :
            bar.value(QStringLiteral("isDay")).toBool() ? m_dayColor : m_nightColor;
        painter->fillRect(QRectF(left, 3, std::max<qreal>(1, right - left), height() - 6), color);
    }
}

} // namespace mcs
