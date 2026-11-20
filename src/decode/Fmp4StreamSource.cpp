#include "decode/Fmp4StreamSource.h"
#include "cache/NasBlockCache.h"
#include "core/Logger.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace mcs {

namespace {
constexpr int kAvioBufferSize = 65536;
} // namespace

Fmp4StreamSource::Fmp4StreamSource(NasBlockCache* cache) : m_cache(cache) {}

Fmp4StreamSource::~Fmp4StreamSource() {
    close();
}

void Fmp4StreamSource::close() {
    if (m_fmtCtx) {
        avformat_close_input(&m_fmtCtx);
        m_fmtCtx = nullptr;
    }
    if (m_avio) {
        av_freep(&m_avio->buffer);
        avio_context_free(&m_avio);
        m_avio = nullptr;
    }
    m_videoStreamIdx = -1;
    m_audioStreamIdx = -1;
    m_sidx.reset();
}

int Fmp4StreamSource::avioReadCallback(void* opaque, uint8_t* buf, int bufSize) {
    return static_cast<Fmp4StreamSource*>(opaque)->readInternal(buf, bufSize);
}

int64_t Fmp4StreamSource::avioSeekCallback(void* opaque, int64_t offset, int whence) {
    return static_cast<Fmp4StreamSource*>(opaque)->seekInternal(offset, whence);
}

int Fmp4StreamSource::readInternal(uint8_t* buf, int bufSize) {
    if (bufSize <= 0) return 0;
    if (m_cancelToken && m_cancelToken->load(std::memory_order_acquire) != m_expectedToken) {
        m_cancelled = true;
        return AVERROR_EXIT;
    }
    if (m_virtualPos >= m_virtualTotalSize) {
        return AVERROR_EOF;
    }

    int totalCopied = 0;

    // 部分 1：虚拟头 (ftyp + moov)
    if (m_isFragmented && m_virtualPos < m_initSize) {
        const size_t avail = static_cast<size_t>(m_initSize - m_virtualPos);
        const size_t toCopy = std::min(avail, static_cast<size_t>(bufSize));
        std::memcpy(buf, m_sidx->initSegment.constData() + m_virtualPos, toCopy);
        m_virtualPos += toCopy;
        totalCopied += static_cast<int>(toCopy);
        if (totalCopied == bufSize || m_virtualPos >= m_virtualTotalSize) {
            return totalCopied;
        }
    }

    // 部分 2：真实媒体区映射
    const int remain = bufSize - totalCopied;
    const uint64_t mediaRelOffset = m_virtualPos - m_initSize;
    const uint64_t realOffset = m_realBaseOffset + mediaRelOffset;
    if (realOffset >= m_realEndOffset) {
        return totalCopied > 0 ? totalCopied : AVERROR_EOF;
    }

    const size_t toRead = static_cast<size_t>(
        std::min<uint64_t>(static_cast<uint64_t>(remain), m_realEndOffset - realOffset));
    std::vector<uint8_t> chunk;
    bool wasCancelled = false;
    const bool ok = m_cache->readRange(
        m_seg.path,
        static_cast<uint64_t>(m_seg.fileSize),
        realOffset,
        toRead,
        chunk,
        IoPriority::Foreground,
        m_cancelToken,
        m_expectedToken,
        &wasCancelled);

    if (wasCancelled) {
        m_cancelled = true;
        return AVERROR_EXIT;
    }
    if (!ok || chunk.empty()) {
        return totalCopied > 0 ? totalCopied : AVERROR_EOF;
    }

    std::memcpy(buf + totalCopied, chunk.data(), chunk.size());
    m_virtualPos += chunk.size();
    totalCopied += static_cast<int>(chunk.size());
    return totalCopied;
}

int64_t Fmp4StreamSource::seekInternal(int64_t offset, int whence) {
    if (whence == AVSEEK_SIZE) {
        return static_cast<int64_t>(m_virtualTotalSize);
    }
    int64_t target = 0;
    if (whence == SEEK_SET) target = offset;
    else if (whence == SEEK_CUR) target = static_cast<int64_t>(m_virtualPos) + offset;
    else if (whence == SEEK_END) target = static_cast<int64_t>(m_virtualTotalSize) + offset;
    else return -1;

    if (target < 0) return -1;
    if (static_cast<uint64_t>(target) > m_virtualTotalSize) {
        target = static_cast<int64_t>(m_virtualTotalSize);
    }
    m_virtualPos = static_cast<uint64_t>(target);
    return target;
}

bool Fmp4StreamSource::open(const SegmentRecord& seg,
                            double targetSecInFile,
                            bool singleFragmentOnly,
                            const std::atomic<uint64_t>* cancelToken,
                            uint64_t expectedToken,
                            bool* outWasCancelled) {
    close();
    if (outWasCancelled) *outWasCancelled = false;
    if (!m_cache) return false;

    const auto t0 = std::chrono::steady_clock::now();
    m_seg = seg;
    m_singleFragmentOnly = singleFragmentOnly;
    m_cancelToken = cancelToken;
    m_expectedToken = expectedToken;
    m_cancelled = false;
    m_ptsCorrectionUs = 0;

    m_sidx = m_cache->getOrLoadSidx(
        seg.path,
        static_cast<uint64_t>(seg.fileSize),
        IoPriority::Foreground,
        cancelToken,
        expectedToken);

    if (cancelToken && cancelToken->load(std::memory_order_acquire) != expectedToken) {
        if (outWasCancelled) *outWasCancelled = true;
        return false;
    }
    if (!m_sidx || !m_sidx->valid) {
        return false;
    }

    if (m_sidx->isFragmented && !m_sidx->entries.empty()) {
        m_isFragmented = true;
        m_fragIndex = m_sidx->findEntryAt(std::max(0.0, targetSecInFile));
        if (m_fragIndex < 0) m_fragIndex = 0;
        const auto& entry = m_sidx->entries[static_cast<size_t>(m_fragIndex)];
        m_fragStartSec = entry.startSec;
        m_initSize = static_cast<uint64_t>(m_sidx->initSegment.size());
        m_realBaseOffset = entry.offset;
        if (singleFragmentOnly) {
            m_realEndOffset = std::min<uint64_t>(
                static_cast<uint64_t>(seg.fileSize), entry.offset + entry.size);
        } else {
            m_realEndOffset = static_cast<uint64_t>(seg.fileSize);
        }
        m_virtualTotalSize = m_initSize + (m_realEndOffset - m_realBaseOffset);
    } else {
        // 标准非分段 MP4 回退
        m_isFragmented = false;
        m_fragIndex = 0;
        m_fragStartSec = 0.0;
        m_initSize = 0;
        m_realBaseOffset = 0;
        m_realEndOffset = static_cast<uint64_t>(seg.fileSize);
        m_virtualTotalSize = m_realEndOffset;
    }
    m_virtualPos = 0;

    auto* avioBuf = static_cast<uint8_t*>(av_malloc(kAvioBufferSize));
    if (!avioBuf) return false;
    m_avio = avio_alloc_context(
        avioBuf, kAvioBufferSize, 0, this, &Fmp4StreamSource::avioReadCallback, nullptr,
        &Fmp4StreamSource::avioSeekCallback);
    if (!m_avio) {
        av_free(avioBuf);
        return false;
    }

    m_fmtCtx = avformat_alloc_context();
    if (!m_fmtCtx) {
        close();
        return false;
    }
    m_fmtCtx->pb = m_avio;
    m_fmtCtx->flags |= AVFMT_FLAG_CUSTOM_IO;
    // 对已带完整 moov 参数的 fMP4 禁止冗余帧率探测，避免额外读取媒体块与打印 probesize 警告
    m_fmtCtx->fps_probe_size = 0;
    m_fmtCtx->probesize = 64 * 1024;
    m_fmtCtx->max_analyze_duration = 100000;

    const int openRc = avformat_open_input(&m_fmtCtx, nullptr, nullptr, nullptr);
    if (m_cancelled || (cancelToken && cancelToken->load(std::memory_order_acquire) != expectedToken)) {
        if (outWasCancelled) *outWasCancelled = true;
        close();
        return false;
    }
    if (openRc < 0) {
        close();
        return false;
    }

    bool needProbeInfo = !m_isFragmented;
    if (!needProbeInfo) {
        bool hasValidVideoPar = false;
        for (unsigned i = 0; i < m_fmtCtx->nb_streams; ++i) {
            const auto* par = m_fmtCtx->streams[i]->codecpar;
            if (par && par->codec_type == AVMEDIA_TYPE_VIDEO && par->width > 0 && par->height > 0) {
                hasValidVideoPar = true;
                break;
            }
        }
        needProbeInfo = !hasValidVideoPar;
    }
    if (needProbeInfo && avformat_find_stream_info(m_fmtCtx, nullptr) < 0) {
        close();
        return false;
    }

    for (unsigned i = 0; i < m_fmtCtx->nb_streams; ++i) {
        const auto type = m_fmtCtx->streams[i]->codecpar->codec_type;
        if (type == AVMEDIA_TYPE_VIDEO && m_videoStreamIdx < 0) {
            m_videoStreamIdx = static_cast<int>(i);
        } else if (type == AVMEDIA_TYPE_AUDIO && m_audioStreamIdx < 0) {
            m_audioStreamIdx = static_cast<int>(i);
        }
    }

    if (m_videoStreamIdx < 0) {
        close();
        return false;
    }

    if (!m_isFragmented && targetSecInFile > 0.5) {
        const int64_t ts = static_cast<int64_t>(targetSecInFile * AV_TIME_BASE);
        av_seek_frame(m_fmtCtx, -1, ts, AVSEEK_FLAG_BACKWARD);
    }

    m_lastIoMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
    return true;
}

bool Fmp4StreamSource::getPrefetchHint(int aheadFragments, uint64_t& outOffset, size_t& outBytes) const {
    if (!m_sidx || !m_isFragmented || m_sidx->entries.empty()) return false;
    const size_t targetIdx = static_cast<size_t>(
        std::clamp(m_fragIndex + aheadFragments, 0, static_cast<int>(m_sidx->entries.size()) - 1));
    const auto& e = m_sidx->entries[targetIdx];
    outOffset = e.offset;
    outBytes = e.size;
    return true;
}

} // namespace mcs
