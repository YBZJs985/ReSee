#pragma once

#include "core/Types.h"

#include <QString>
#include <cstdint>
#include <memory>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
}

struct SwsContext;

namespace mcs {

// 跨平台硬件解码辅助类：
// Windows -> AV_HWDEVICE_TYPE_D3D11VA
// macOS   -> AV_HWDEVICE_TYPE_VIDEOTOOLBOX
class HwAccelContext {
public:
    HwAccelContext() = default;
    ~HwAccelContext();

    HwAccelContext(const HwAccelContext&) = delete;
    HwAccelContext& operator=(const HwAccelContext&) = delete;

    bool init(bool preferHardware);
    void release();

    bool isHardwareReady() const { return m_hwDeviceCtx != nullptr; }
    AVHWDeviceType deviceType() const { return m_deviceType; }
    AVPixelFormat hwPixFmt() const { return m_hwPixFmt; }
    QString backendName() const;

    // 绑定到指定的 AVCodecContext
    bool attachToCodec(AVCodecContext* codecCtx);

    // 将硬解帧（或软解帧）统一转换为紧凑 NV12 平面数据（全平台 GPU 着色器统一输入）
    // 支持传入固定缓冲池或分配新 vector，避免频繁重分配
    bool extractNv12Frame(AVFrame* frame,
                          int& outWidth,
                          int& outHeight,
                          int& outYStride,
                          int& outUvStride,
                          std::shared_ptr<std::vector<uint8_t>>& outBuffer,
                          bool& outIsHardware);

private:
    static enum AVPixelFormat getHwFormatCallback(AVCodecContext* ctx, const enum AVPixelFormat* pixFmts);

    AVBufferRef* m_hwDeviceCtx = nullptr;
    AVHWDeviceType m_deviceType = AV_HWDEVICE_TYPE_NONE;
    AVPixelFormat m_hwPixFmt = AV_PIX_FMT_NONE;
    AVFrame* m_swFrame = nullptr;
    ::SwsContext* m_swsCtx = nullptr;
    int m_swsW = 0;
    int m_swsH = 0;
    int m_swsFmt = AV_PIX_FMT_NONE;
};

} // namespace mcs
