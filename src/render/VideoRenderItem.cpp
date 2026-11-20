#include "render/VideoRenderItem.h"
#include "core/TimeUtils.h"

#include <QPointer>
#include <QQuickWindow>
#include <QSGGeometry>
#include <QSGGeometryNode>
#include <QSGMaterial>
#include <QSGMaterialShader>
#include <QSGTexture>
#include <rhi/qrhi.h>
#include <algorithm>
#include <cstring>

namespace mcs {

namespace {

struct Nv12Uniforms {
    float qtMatrix[16];
    float qtOpacity = 1.0f;
    float dimFactor = 1.0f;
    float isPreview = 0.0f;
    float pad0 = 0.0f;
};
static_assert(sizeof(Nv12Uniforms) == 80, "std140 uniform size mismatch");

class Nv12Material;

class Nv12Shader : public QSGMaterialShader {
public:
    Nv12Shader() {
        setShaderFileName(VertexStage, QStringLiteral(":/shaders/shaders/nv12.vert.qsb"));
        setShaderFileName(FragmentStage, QStringLiteral(":/shaders/shaders/nv12.frag.qsb"));
    }

    bool updateUniformData(RenderState& state, QSGMaterial* newMaterial, QSGMaterial* oldMaterial) override;
    void updateSampledImage(RenderState& state,
                            int binding,
                            QSGTexture** texture,
                            QSGMaterial* newMaterial,
                            QSGMaterial* oldMaterial) override;
};

class Nv12Material : public QSGMaterial {
public:
    Nv12Material() {
        setFlag(Blending, true);
    }

    ~Nv12Material() override {
        sgY.reset();
        sgUV.reset();
    }

    QSGMaterialType* type() const override {
        static QSGMaterialType typeId;
        return &typeId;
    }

    QSGMaterialShader* createShader(QSGRendererInterface::RenderMode) const override {
        return new Nv12Shader();
    }

    int compare(const QSGMaterial* other) const override {
        const auto* o = static_cast<const Nv12Material*>(other);
        if (o == this) return 0;
        return this < o ? -1 : 1;
    }

    QPointer<QQuickWindow> window;
    std::shared_ptr<const DecodedFrame> frame;
    uint64_t frameSeq = 0;
    uint64_t uploadedSeq = 0;

    int texWidth = 0;
    int texHeight = 0;
    QRhiTexture* rhiY = nullptr;
    QRhiTexture* rhiUV = nullptr;
    std::unique_ptr<QSGTexture> sgY;
    std::unique_ptr<QSGTexture> sgUV;

    float dimFactor = 1.0f;
    float isPreview = 0.0f;
};

bool Nv12Shader::updateUniformData(RenderState& state, QSGMaterial* newMaterial, QSGMaterial*) {
    auto* mat = static_cast<Nv12Material*>(newMaterial);
    QByteArray* buf = state.uniformData();
    if (buf->size() < static_cast<qsizetype>(sizeof(Nv12Uniforms))) {
        return false;
    }

    Nv12Uniforms u{};
    std::memcpy(u.qtMatrix, state.combinedMatrix().constData(), 64);
    u.qtOpacity = state.opacity();
    u.dimFactor = mat->dimFactor;
    u.isPreview = mat->isPreview;
    std::memcpy(buf->data(), &u, sizeof(Nv12Uniforms));
    return true;
}

void Nv12Shader::updateSampledImage(RenderState& state,
                                    int binding,
                                    QSGTexture** texture,
                                    QSGMaterial* newMaterial,
                                    QSGMaterial*) {
    auto* mat = static_cast<Nv12Material*>(newMaterial);
    if (!mat || !mat->frame || !mat->frame->nv12Data || !mat->window) {
        return;
    }

    QRhi* rhi = state.rhi();
    QRhiResourceUpdateBatch* batch = state.resourceUpdateBatch();
    if (!rhi || !batch) return;

    const auto& f = *mat->frame;
    const int w = f.width;
    const int h = f.height;
    const int uvW = (w + 1) / 2;
    const int uvH = (h + 1) / 2;
    if (w <= 0 || h <= 0) return;

    if (!mat->rhiY || !mat->rhiUV || mat->texWidth != w || mat->texHeight != h) {
        mat->sgY.reset();
        mat->sgUV.reset();
        mat->rhiY = rhi->newTexture(QRhiTexture::R8, QSize(w, h));
        mat->rhiUV = rhi->newTexture(QRhiTexture::RG8, QSize(uvW, uvH));
        if (!mat->rhiY->create() || !mat->rhiUV->create()) {
            delete mat->rhiY;
            delete mat->rhiUV;
            mat->rhiY = nullptr;
            mat->rhiUV = nullptr;
            return;
        }
        mat->sgY.reset(mat->window->createTextureFromRhiTexture(mat->rhiY, QQuickWindow::TextureOwnsGLTexture));
        mat->sgUV.reset(mat->window->createTextureFromRhiTexture(mat->rhiUV, QQuickWindow::TextureOwnsGLTexture));
        if (mat->sgY) mat->sgY->setFiltering(QSGTexture::Linear);
        if (mat->sgUV) mat->sgUV->setFiltering(QSGTexture::Linear);
        mat->texWidth = w;
        mat->texHeight = h;
        mat->uploadedSeq = 0;
    }

    if (mat->uploadedSeq != mat->frameSeq) {
        const size_t yBytes = static_cast<size_t>(f.yStride) * h;
        const size_t uvBytes = static_cast<size_t>(f.uvStride) * uvH;
        if (f.nv12Data->size() >= yBytes + uvBytes) {
            const uint8_t* ptrY = f.nv12Data->data();
            const uint8_t* ptrUV = ptrY + yBytes;

            QRhiTextureSubresourceUploadDescription descY(ptrY, static_cast<quint32>(yBytes));
            descY.setDataStride(static_cast<quint32>(f.yStride));
            batch->uploadTexture(mat->rhiY, QRhiTextureUploadDescription(QRhiTextureUploadEntry(0, 0, descY)));

            QRhiTextureSubresourceUploadDescription descUV(ptrUV, static_cast<quint32>(uvBytes));
            descUV.setDataStride(static_cast<quint32>(f.uvStride));
            batch->uploadTexture(mat->rhiUV, QRhiTextureUploadDescription(QRhiTextureUploadEntry(0, 0, descUV)));

            mat->uploadedSeq = mat->frameSeq;
        }
    }

    if (binding == 1 && mat->sgY) {
        *texture = mat->sgY.get();
    } else if (binding == 2 && mat->sgUV) {
        *texture = mat->sgUV.get();
    }
}

} // namespace

VideoRenderItem::VideoRenderItem(QQuickItem* parent) : QQuickItem(parent) {
    setFlag(ItemHasContents, true);
    setClip(true);
}

void VideoRenderItem::setStreamIndex(int idx) {
    if (m_streamIndex == idx) return;
    m_streamIndex = idx;
    emit streamIndexChanged();
}

void VideoRenderItem::setZoomScale(double s) {
    const double clamped = std::clamp(s, 1.0, 8.0);
    if (std::abs(clamped - m_zoomScale) < 1e-4) return;
    m_zoomScale = clamped;
    clampPan();
    m_geometryDirty = true;
    emit zoomChanged();
    update();
}

void VideoRenderItem::setPanX(double x) {
    m_panX = x;
    clampPan();
    m_geometryDirty = true;
    emit zoomChanged();
    update();
}

void VideoRenderItem::setPanY(double y) {
    m_panY = y;
    clampPan();
    m_geometryDirty = true;
    emit zoomChanged();
    update();
}

void VideoRenderItem::clampPan() {
    if (m_zoomScale <= 1.001) {
        m_panX = 0.0;
        m_panY = 0.0;
        return;
    }
    const double maxPan = 0.5 - (0.5 / m_zoomScale);
    m_panX = std::clamp(m_panX, -maxPan, maxPan);
    m_panY = std::clamp(m_panY, -maxPan, maxPan);
}

void VideoRenderItem::zoomAt(double factor, double normX, double normY) {
    const double oldZoom = m_zoomScale;
    const double newZoom = std::clamp(oldZoom * factor, 1.0, 8.0);
    if (std::abs(newZoom - oldZoom) < 1e-4) return;

    // 保持鼠标归一化坐标 (normX, normY) 对应的纹理坐标不变
    const double nx = std::clamp(normX, 0.0, 1.0);
    const double ny = std::clamp(normY, 0.0, 1.0);
    const double oldSpan = 1.0 / oldZoom;
    const double newSpan = 1.0 / newZoom;
    const double texX = (0.5 - m_panX) + (nx - 0.5) * oldSpan;
    const double texY = (0.5 - m_panY) + (ny - 0.5) * oldSpan;

    const double newCenterX = texX - (nx - 0.5) * newSpan;
    const double newCenterY = texY - (ny - 0.5) * newSpan;

    m_zoomScale = newZoom;
    m_panX = 0.5 - newCenterX;
    m_panY = 0.5 - newCenterY;
    clampPan();
    m_geometryDirty = true;
    emit zoomChanged();
    update();
}

void VideoRenderItem::panBy(double deltaItemX, double deltaItemY) {
    if (m_zoomScale <= 1.001 || width() <= 1.0 || height() <= 1.0) return;
    const double dNormX = deltaItemX / width();
    const double dNormY = deltaItemY / height();
    m_panX += dNormX / m_zoomScale;
    m_panY += dNormY / m_zoomScale;
    clampPan();
    m_geometryDirty = true;
    emit zoomChanged();
    update();
}

void VideoRenderItem::resetZoom() {
    if (m_zoomScale == 1.0 && m_panX == 0.0 && m_panY == 0.0) return;
    m_zoomScale = 1.0;
    m_panX = 0.0;
    m_panY = 0.0;
    m_geometryDirty = true;
    emit zoomChanged();
    update();
}

void VideoRenderItem::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) {
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    m_geometryDirty = true;
    update();
}

void VideoRenderItem::updateFrame(const std::shared_ptr<const DecodedFrame>& frame,
                                  DecodeState state,
                                  int tzOffsetSec,
                                  int64_t waitDurationMs) {
    bool changed = false;
    {
        QMutexLocker lk(&m_frameMtx);
        if (state == DecodeState::NoVideo || state == DecodeState::Corrupt ||
            state == DecodeState::Disconnected || state == DecodeState::Indexing) {
            if (m_pendingFrame) {
                m_pendingFrame.reset();
                ++m_frameSeq;
            }
        } else if (frame && frame != m_pendingFrame) {
            m_pendingFrame = frame;
            ++m_frameSeq;
        }
    }

    const bool hasV = (frame && frame->nv12Data &&
                       state != DecodeState::NoVideo &&
                       state != DecodeState::Corrupt &&
                       state != DecodeState::Disconnected &&
                       state != DecodeState::Indexing);
    const bool prev = hasV && frame->isPreview;
    const bool hw = hasV && frame->isHardware;
    const int vw = hasV ? frame->width : 0;
    const int vh = hasV ? frame->height : 0;
    const QString timeTxt = hasV ? TimeUtils::formatEpochMs(frame->wallTimeMs, tzOffsetSec) : QString();
    const qint64 waitMs = static_cast<qint64>(std::max<int64_t>(0, waitDurationMs));

    if (m_hasVideo != hasV || m_isPreview != prev || m_isHardware != hw ||
        m_videoWidth != vw || m_videoHeight != vh ||
        m_decodeState != state || m_videoTimeText != timeTxt ||
        m_waitDurationMs != waitMs) {
        m_hasVideo = hasV;
        m_isPreview = prev;
        m_isHardware = hw;
        m_videoWidth = vw;
        m_videoHeight = vh;
        m_decodeState = state;
        m_videoTimeText = timeTxt;
        m_waitDurationMs = waitMs;
        changed = true;
    }

    if (changed) {
        emit frameStateChanged();
    }
    update();
}

QSGNode* VideoRenderItem::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) {
    std::shared_ptr<const DecodedFrame> frame;
    uint64_t seq = 0;
    {
        QMutexLocker lk(&m_frameMtx);
        frame = m_pendingFrame;
        seq = m_frameSeq;
    }

    if (!frame || !frame->nv12Data || frame->width <= 0 || frame->height <= 0 ||
        width() <= 0 || height() <= 0) {
        delete oldNode;
        return nullptr;
    }

    auto* node = static_cast<QSGGeometryNode*>(oldNode);
    if (!node) {
        node = new QSGGeometryNode();
        auto* geom = new QSGGeometry(QSGGeometry::defaultAttributes_TexturedPoint2D(), 4);
        geom->setDrawingMode(QSGGeometry::DrawTriangleStrip);
        node->setGeometry(geom);
        node->setFlag(QSGNode::OwnsGeometry);

        auto* mat = new Nv12Material();
        mat->window = window();
        node->setMaterial(mat);
        node->setFlag(QSGNode::OwnsMaterial);
        m_geometryDirty = true;
    }

    auto* mat = static_cast<Nv12Material*>(node->material());
    mat->window = window();
    if (mat->frameSeq != seq) {
        mat->frame = frame;
        mat->frameSeq = seq;
        mat->dimFactor = frame->isPreview ? 0.85f : 1.0f;
        mat->isPreview = frame->isPreview ? 1.0f : 0.0f;
        node->markDirty(QSGNode::DirtyMaterial);
    }

    // 计算保持画面比例 (PreserveAspectFit) 的渲染矩形 + 局部缩放平移纹理坐标
    const float itemW = static_cast<float>(width());
    const float itemH = static_cast<float>(height());
    const float vidAspect = static_cast<float>(frame->width) / static_cast<float>(frame->height);
    const float itemAspect = itemW / itemH;

    float drawW = itemW;
    float drawH = itemH;
    float drawX = 0.0f;
    float drawY = 0.0f;
    if (itemAspect > vidAspect) {
        drawW = itemH * vidAspect;
        drawX = (itemW - drawW) * 0.5f;
    } else {
        drawH = itemW / vidAspect;
        drawY = (itemH - drawH) * 0.5f;
    }

    const float halfSpan = static_cast<float>(0.5 / m_zoomScale);
    const float cx = std::clamp(static_cast<float>(0.5 - m_panX), halfSpan, 1.0f - halfSpan);
    const float cy = std::clamp(static_cast<float>(0.5 - m_panY), halfSpan, 1.0f - halfSpan);
    const float u0 = cx - halfSpan;
    const float u1 = cx + halfSpan;
    const float v0 = cy - halfSpan;
    const float v1 = cy + halfSpan;

    auto* verts = node->geometry()->vertexDataAsTexturedPoint2D();
    verts[0].set(drawX, drawY, u0, v0);
    verts[1].set(drawX + drawW, drawY, u1, v0);
    verts[2].set(drawX, drawY + drawH, u0, v1);
    verts[3].set(drawX + drawW, drawY + drawH, u1, v1);
    node->markDirty(QSGNode::DirtyGeometry);
    m_geometryDirty = false;

    return node;
}

} // namespace mcs
