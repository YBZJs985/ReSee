#pragma once

#include <QColor>
#include <QPointer>
#include <QQuickPaintedItem>
#include <QtQml/qqmlregistration.h>

#include "app/TimelineModel.h"

namespace mcs {

class TimelineTrackItem : public QQuickPaintedItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QObject* model READ model WRITE setModel NOTIFY modelChanged)
    Q_PROPERTY(int stream READ stream WRITE setStream NOTIFY streamChanged)
    Q_PROPERTY(QColor dayColor MEMBER m_dayColor NOTIFY colorsChanged)
    Q_PROPERTY(QColor nightColor MEMBER m_nightColor NOTIFY colorsChanged)
    Q_PROPERTY(QColor writingColor MEMBER m_writingColor NOTIFY colorsChanged)
    Q_PROPERTY(QColor errorColor MEMBER m_errorColor NOTIFY colorsChanged)

public:
    explicit TimelineTrackItem(QQuickItem* parent = nullptr);

    QObject* model() const { return m_model; }
    void setModel(QObject* model);
    int stream() const { return m_stream; }
    void setStream(int stream);
    void paint(QPainter* painter) override;

signals:
    void modelChanged();
    void streamChanged();
    void colorsChanged();

protected:
    void updatePolish() override;

private:
    QPointer<TimelineModel> m_model;
    QVariantList m_bars;
    QList<QPointF> m_nightBands;
    int m_stream = -1;
    QColor m_dayColor;
    QColor m_nightColor;
    QColor m_writingColor;
    QColor m_errorColor;
};

} // namespace mcs
