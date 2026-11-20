#pragma once

#include "core/Types.h"

#include <QMutex>
#include <QQuickItem>
#include <memory>

namespace mcs {

class VideoRenderItem : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(int streamIndex READ streamIndex WRITE setStreamIndex NOTIFY streamIndexChanged)
    Q_PROPERTY(double zoomScale READ zoomScale WRITE setZoomScale NOTIFY zoomChanged)
    Q_PROPERTY(double panX READ panX WRITE setPanX NOTIFY zoomChanged)
    Q_PROPERTY(double panY READ panY WRITE setPanY NOTIFY zoomChanged)
    Q_PROPERTY(bool hasVideo READ hasVideo NOTIFY frameStateChanged)
    Q_PROPERTY(bool isPreview READ isPreview NOTIFY frameStateChanged)
    Q_PROPERTY(bool isHardware READ isHardware NOTIFY frameStateChanged)
    Q_PROPERTY(int videoWidth READ videoWidth NOTIFY frameStateChanged)
    Q_PROPERTY(int videoHeight READ videoHeight NOTIFY frameStateChanged)
    Q_PROPERTY(int decodeStateCode READ decodeStateCode NOTIFY frameStateChanged)
    Q_PROPERTY(QString decodeStateText READ decodeStateText NOTIFY frameStateChanged)
    Q_PROPERTY(QString videoTimeText READ videoTimeText NOTIFY frameStateChanged)
    Q_PROPERTY(bool isBuffering READ isBuffering NOTIFY frameStateChanged)
    Q_PROPERTY(bool isLocating READ isLocating NOTIFY frameStateChanged)
    Q_PROPERTY(qint64 waitDurationMs READ waitDurationMs NOTIFY frameStateChanged)

public:
    explicit VideoRenderItem(QQuickItem* parent = nullptr);
    ~VideoRenderItem() override = default;

    int streamIndex() const { return m_streamIndex; }
    void setStreamIndex(int idx);

    double zoomScale() const { return m_zoomScale; }
    void setZoomScale(double s);

    double panX() const { return m_panX; }
    void setPanX(double x);

    double panY() const { return m_panY; }
    void setPanY(double y);

    bool hasVideo() const { return m_hasVideo; }
    bool isPreview() const { return m_isPreview; }
    bool isHardware() const { return m_isHardware; }
    int videoWidth() const { return m_videoWidth; }
    int videoHeight() const { return m_videoHeight; }
    int decodeStateCode() const { return static_cast<int>(m_decodeState); }
    QString decodeStateText() const { return decodeStateLabel(m_decodeState); }
    QString videoTimeText() const { return m_videoTimeText; }
    bool isBuffering() const { return m_decodeState == DecodeState::Buffering; }
    bool isLocating() const { return m_decodeState == DecodeState::Locating; }
    qint64 waitDurationMs() const { return m_waitDurationMs; }

    Q_INVOKABLE void zoomAt(double factor, double normX, double normY);
    Q_INVOKABLE void panBy(double deltaItemX, double deltaItemY);
    Q_INVOKABLE void resetZoom();

    // 由 PlaybackController 在主线程调用，向 GPU 管线提交最新 NV12 帧
    void updateFrame(const std::shared_ptr<const DecodedFrame>& frame,
                     DecodeState state,
                     int tzOffsetSec,
                     int64_t waitDurationMs = 0);

signals:
    void streamIndexChanged();
    void zoomChanged();
    void frameStateChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData* data) override;
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;

private:
    void clampPan();

    int m_streamIndex = 0;
    double m_zoomScale = 1.0;
    double m_panX = 0.0; // UV 中心偏移 [-0.5, 0.5]
    double m_panY = 0.0;

    bool m_hasVideo = false;
    bool m_isPreview = false;
    bool m_isHardware = false;
    int m_videoWidth = 0;
    int m_videoHeight = 0;
    DecodeState m_decodeState = DecodeState::Idle;
    QString m_videoTimeText;
    qint64 m_waitDurationMs = 0;

    mutable QMutex m_frameMtx;
    std::shared_ptr<const DecodedFrame> m_pendingFrame;
    uint64_t m_frameSeq = 0;
    bool m_geometryDirty = true;
};

} // namespace mcs
