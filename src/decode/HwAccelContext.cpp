#include "decode/HwAccelContext.h"
#include "core/Logger.h"

#include <algorithm>
#include <cstring>

extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace mcs {

HwAccelContext::~HwAccelContext() {
    release();
}

void HwAccelContext::release() {
    if (m_swsCtx) {
        sws_freeContext(m_swsCtx);
        m_swsCtx = nullptr;
    }
    m_swsW = 0;
    m_swsH = 0;
    m_swsFmt = AV_PIX_FMT_NONE;
    if (m_swFrame) {
        av_frame_free(&m_swFrame);
    }
    if (m_hwDeviceCtx) {
        av_buffer_unref(&m_hwDeviceCtx);
    }
    m_deviceType = AV_HWDEVICE_TYPE_NONE;
    m_hwPixFmt = AV_PIX_FMT_NONE;
}

bool HwAccelContext::init(bool preferHardware) {
    release();
    m_swFrame = av_frame_alloc();
    if (!preferHardware) {
        return true;
    }

#if defined(SYNC_PLATFORM_WINDOWS)
    const AVHWDeviceType candidates[] = {AV_HWDEVICE_TYPE_D3D11VA, AV_HWDEVICE_TYPE_CUDA};
#elif defined(SYNC_PLATFORM_MACOS)
    const AVHWDeviceType candidates[] = {AV_HWDEVICE_TYPE_VIDEOTOOLBOX};
#else
    const AVHWDeviceType candidates[] = {AV_HWDEVICE_TYPE_VAAPI, AV_HWDEVICE_TYPE_VDPAU};
#endif

    for (AVHWDeviceType type : candidates) {
        if (av_hwdevice_ctx_create(&m_hwDeviceCtx, type, nullptr, nullptr, 0) >= 0) {
            m_deviceType = type;
#if defined(SYNC_PLATFORM_WINDOWS)
            m_hwPixFmt = (type == AV_HWDEVICE_TYPE_D3D11VA) ? AV_PIX_FMT_D3D11 : AV_PIX_FMT_CUDA;
#elif defined(SYNC_PLATFORM_MACOS)
            m_hwPixFmt = AV_PIX_FMT_VIDEOTOOLBOX;
#else
            m_hwPixFmt = AV_PIX_FMT_VAAPI;
#endif
            MCS_LOG_INFO("HWDEC", QStringLiteral("硬件解码初始化成功: %1").arg(backendName()));
            return true;
        }
    }

    MCS_LOG_WARN("HWDEC", QStringLiteral("硬件解码初始化失败，已降级为 CPU 软解"));
    return true;
}

QString HwAccelContext::backendName() const {
    if (!m_hwDeviceCtx) return QStringLiteral("SW");
    const char* n = av_hwdevice_get_type_name(m_deviceType);
    return n ? QString::fromLatin1(n).toUpper() : QStringLiteral("HW");
}

enum AVPixelFormat HwAccelContext::getHwFormatCallback(AVCodecContext* ctx, const enum AVPixelFormat* pixFmts) {
    auto* self = static_cast<HwAccelContext*>(ctx->opaque);
    if (self && self->m_hwPixFmt != AV_PIX_FMT_NONE) {
        for (const enum AVPixelFormat* p = pixFmts; *p != AV_PIX_FMT_NONE; ++p) {
            if (*p == self->m_hwPixFmt) {
                return *p;
            }
        }
    }
    for (const enum AVPixelFormat* p = pixFmts; *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == AV_PIX_FMT_NV12 || *p == AV_PIX_FMT_YUV420P) {
            return *p;
        }
    }
    return pixFmts[0];
}

bool HwAccelContext::attachToCodec(AVCodecContext* codecCtx) {
    if (!codecCtx) return false;
    if (!m_hwDeviceCtx) return false;
    codecCtx->opaque = this;
    codecCtx->hw_device_ctx = av_buffer_ref(m_hwDeviceCtx);
    codecCtx->get_format = &HwAccelContext::getHwFormatCallback;
    return true;
}

bool HwAccelContext::extractNv12Frame(AVFrame* frame,
                                      int& outWidth,
                                      int& outHeight,
                                      int& outYStride,
                                      int& outUvStride,
                                      std::shared_ptr<std::vector<uint8_t>>& outBuffer,
                                      bool& outIsHardware) {
    if (!frame || frame->width <= 0 || frame->height <= 0) return false;

    AVFrame* src = frame;
    outIsHardware = false;

    if (m_hwPixFmt != AV_PIX_FMT_NONE && frame->format == m_hwPixFmt) {
        av_frame_unref(m_swFrame);
        if (av_hwframe_transfer_data(m_swFrame, frame, 0) < 0) {
            return false;
        }
        src = m_swFrame;
        outIsHardware = true;
    }

    const int w = src->width;
    const int h = src->height;
    const int uvH = (h + 1) / 2;
    const int yStride = w;
    // 确保奇数宽度时 UV 交错行步长向上对齐到偶数，防止写越界
    const int uvStride = ((w + 1) / 2) * 2;
    const size_t totalBytes = static_cast<size_t>(yStride) * h + static_cast<size_t>(uvStride) * uvH;

    auto buf = std::make_shared<std::vector<uint8_t>>(totalBytes);
    uint8_t* dstY = buf->data();
    uint8_t* dstUV = dstY + static_cast<size_t>(yStride) * h;

    if (src->format == AV_PIX_FMT_NV12) {
        for (int row = 0; row < h; ++row) {
            std::memcpy(dstY + static_cast<size_t>(row) * yStride,
                        src->data[0] + static_cast<size_t>(row) * src->linesize[0],
                        static_cast<size_t>(w));
        }
        for (int row = 0; row < uvH; ++row) {
            std::memcpy(dstUV + static_cast<size_t>(row) * uvStride,
                        src->data[1] + static_cast<size_t>(row) * src->linesize[1],
                        static_cast<size_t>(w));
        }
    } else if (src->format == AV_PIX_FMT_YUV420P || src->format == AV_PIX_FMT_YUVJ420P) {
        // 常见软解输出 YUV420P -> 高速打包为 NV12（避免 sws_scale 开销）
        for (int row = 0; row < h; ++row) {
            std::memcpy(dstY + static_cast<size_t>(row) * yStride,
                        src->data[0] + static_cast<size_t>(row) * src->linesize[0],
                        static_cast<size_t>(w));
        }
        const int chromaW = (w + 1) / 2;
        for (int row = 0; row < uvH; ++row) {
            const uint8_t* uRow = src->data[1] + static_cast<size_t>(row) * src->linesize[1];
            const uint8_t* vRow = src->data[2] + static_cast<size_t>(row) * src->linesize[2];
            uint8_t* uvOut = dstUV + static_cast<size_t>(row) * uvStride;
            for (int col = 0; col < chromaW; ++col) {
                uvOut[col * 2] = uRow[col];
                uvOut[col * 2 + 1] = vRow[col];
            }
        }
    } else {
        // 其他像素格式回退 sws_scale 到 NV12
        if (!m_swsCtx || m_swsW != w || m_swsH != h || m_swsFmt != src->format) {
            if (m_swsCtx) sws_freeContext(m_swsCtx);
            m_swsCtx = sws_getContext(w, h, static_cast<AVPixelFormat>(src->format),
                                      w, h, AV_PIX_FMT_NV12,
                                      SWS_BILINEAR, nullptr, nullptr, nullptr);
            m_swsW = w;
            m_swsH = h;
            m_swsFmt = src->format;
        }
        if (!m_swsCtx) return false;
        uint8_t* dstPlanes[4] = {dstY, dstUV, nullptr, nullptr};
        int dstLinesizes[4] = {yStride, uvStride, 0, 0};
        sws_scale(m_swsCtx, src->data, src->linesize, 0, h, dstPlanes, dstLinesizes);
    }

    outWidth = w;
    outHeight = h;
    outYStride = yStride;
    outUvStride = uvStride;
    outBuffer = std::move(buf);
    return true;
}

} // namespace mcs
