#pragma once

#include "core/Types.h"
#include "index/SidxParser.h"

#include <QString>
#include <atomic>
#include <cstdint>
#include <memory>

extern "C" {
#include <libavformat/avformat.h>
}

namespace mcs {

class NasBlockCache;

// 通过自定义 AVIOContext 封装 NasBlockCache，实现：
// 1. fMP4 + sidx 模式下构建虚拟字节流 [ftyp+moov] + [目标 fragment 起的连续字节]，瞬间定位；
// 2. 支持单片段模式 (singleFragmentOnly=true)，供极速穿梭预览使用，只读单片段；
// 3. 支持标准 MP4 回退（当非 sidx 分段文件时）；
// 4. 实时响应 cancelToken，在任何 NAS 读取阶段可毫秒级取消。
class Fmp4StreamSource {
public:
    explicit Fmp4StreamSource(NasBlockCache* cache);
    ~Fmp4StreamSource();

    Fmp4StreamSource(const Fmp4StreamSource&) = delete;
    Fmp4StreamSource& operator=(const Fmp4StreamSource&) = delete;

    bool open(const SegmentRecord& seg,
              double targetSecInFile,
              bool singleFragmentOnly,
              const std::atomic<uint64_t>* cancelToken,
              uint64_t expectedToken,
              bool* outWasCancelled = nullptr);

    void close();

    AVFormatContext* formatContext() const { return m_fmtCtx; }
    int videoStreamIndex() const { return m_videoStreamIdx; }
    int audioStreamIndex() const { return m_audioStreamIdx; }
    double openedFragmentStartSec() const { return m_fragStartSec; }
    int64_t ptsCorrectionUs() const { return m_ptsCorrectionUs; }
    bool isFragmented() const { return m_isFragmented; }
    double lastNasIoMs() const { return m_lastIoMs; }

    // 获取当前正在播放文件的下一个（或指定偏移）fragment 磁盘位置，供后台预读用
    bool getPrefetchHint(int aheadFragments, uint64_t& outOffset, size_t& outBytes) const;

private:
    static int avioReadCallback(void* opaque, uint8_t* buf, int bufSize);
    static int64_t avioSeekCallback(void* opaque, int64_t offset, int whence);

    int readInternal(uint8_t* buf, int bufSize);
    int64_t seekInternal(int64_t offset, int whence);

    NasBlockCache* m_cache = nullptr;
    SegmentRecord m_seg;
    std::shared_ptr<const SidxIndex> m_sidx;

    bool m_isFragmented = false;
    bool m_singleFragmentOnly = false;
    int m_fragIndex = 0;
    double m_fragStartSec = 0.0;
    int64_t m_ptsCorrectionUs = 0; // 仅当文件无 tfdt 导致首帧 PTS 归零时用于兜底修正

    // 虚拟流布局：[0 .. initSize) 来自 initSegment；[initSize .. virtualTotalSize) 映射真实文件 [realBaseOffset .. realEndOffset)
    uint64_t m_initSize = 0;
    uint64_t m_realBaseOffset = 0;
    uint64_t m_realEndOffset = 0;
    uint64_t m_virtualTotalSize = 0;
    uint64_t m_virtualPos = 0;

    const std::atomic<uint64_t>* m_cancelToken = nullptr;
    uint64_t m_expectedToken = 0;
    bool m_cancelled = false;
    double m_lastIoMs = 0.0;

    AVIOContext* m_avio = nullptr;
    AVFormatContext* m_fmtCtx = nullptr;
    int m_videoStreamIdx = -1;
    int m_audioStreamIdx = -1;
};

} // namespace mcs
