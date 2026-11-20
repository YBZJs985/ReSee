#include "index/SidxParser.h"

#include <algorithm>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

namespace mcs {

namespace {

inline uint16_t readU16BE(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

inline uint32_t readU32BE(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

inline uint64_t readU64BE(const uint8_t* p) {
    return (static_cast<uint64_t>(readU32BE(p)) << 32) | readU32BE(p + 4);
}

inline bool tagEq(const uint8_t* p, const char* tag) {
    return std::memcmp(p, tag, 4) == 0;
}

struct MemBufferCursor {
    const uint8_t* data = nullptr;
    size_t size = 0;
    size_t pos = 0;
};

int memReadPacket(void* opaque, uint8_t* buf, int bufSize) {
    auto* cur = static_cast<MemBufferCursor*>(opaque);
    if (!cur || cur->pos >= cur->size) return AVERROR_EOF;
    const size_t avail = cur->size - cur->pos;
    const size_t toCopy = std::min(avail, static_cast<size_t>(bufSize));
    std::memcpy(buf, cur->data + cur->pos, toCopy);
    cur->pos += toCopy;
    return static_cast<int>(toCopy);
}

int64_t memSeekPacket(void* opaque, int64_t offset, int whence) {
    auto* cur = static_cast<MemBufferCursor*>(opaque);
    if (!cur) return -1;
    if (whence == AVSEEK_SIZE) {
        return static_cast<int64_t>(cur->size);
    }
    int64_t target = 0;
    if (whence == SEEK_SET) target = offset;
    else if (whence == SEEK_CUR) target = static_cast<int64_t>(cur->pos) + offset;
    else if (whence == SEEK_END) target = static_cast<int64_t>(cur->size) + offset;
    else return -1;
    if (target < 0 || static_cast<size_t>(target) > cur->size) return -1;
    cur->pos = static_cast<size_t>(target);
    return target;
}

} // namespace

int SidxIndex::findEntryAt(double targetSec) const {
    if (entries.empty()) return -1;
    if (targetSec <= 0.0) return 0;
    auto it = std::upper_bound(
        entries.begin(), entries.end(), targetSec,
        [](double t, const SidxEntry& e) { return t < e.startSec; });
    if (it == entries.begin()) return 0;
    --it;
    return static_cast<int>(std::distance(entries.begin(), it));
}

SidxIndex SidxParser::parseHeaderBytes(const uint8_t* data, size_t len, uint64_t totalFileSize) {
    SidxIndex res;
    if (!data || len < 16 || totalFileSize < 16) {
        return res;
    }

    bool sawFtyp = false;
    bool sawMoov = false;
    bool sawSidx = false;
    bool sawMoof = false;

    size_t pos = 0;
    while (pos + 8 <= len) {
        uint64_t boxSize = readU32BE(data + pos);
        const uint8_t* typePtr = data + pos + 4;
        size_t hdrSize = 8;

        if (boxSize == 1) {
            if (pos + 16 > len) break;
            boxSize = readU64BE(data + pos + 8);
            hdrSize = 16;
        } else if (boxSize == 0) {
            boxSize = totalFileSize - pos;
        }

        if (boxSize < hdrSize || pos + boxSize > totalFileSize) {
            // 损坏 box
            break;
        }

        // 如果 box 头部在当前缓冲末尾被截断，请求扩大读取窗口
        if (pos + boxSize > len) {
            if (tagEq(typePtr, "moov") || tagEq(typePtr, "sidx") || tagEq(typePtr, "ftyp")) {
                res.needMoreHeader = true;
                res.suggestedHeaderBytes = std::min<uint64_t>(pos + boxSize + 65536ULL, 4ULL * 1024ULL * 1024ULL);
            }
            break;
        }

        if (tagEq(typePtr, "ftyp")) {
            sawFtyp = true;
            res.initSegment.append(reinterpret_cast<const char*>(data + pos), static_cast<qsizetype>(boxSize));
        } else if (tagEq(typePtr, "moov")) {
            sawMoov = true;
            res.initSegment.append(reinterpret_cast<const char*>(data + pos), static_cast<qsizetype>(boxSize));
        } else if (tagEq(typePtr, "sidx")) {
            // 解析 ISO BMFF sidx box
            if (boxSize >= hdrSize + 12) {
                const uint8_t* body = data + pos + hdrSize;
                const size_t bodyLen = static_cast<size_t>(boxSize - hdrSize);
                size_t off = 0;
                const uint8_t version = body[0];
                off += 4; // version(1) + flags(3)
                if (off + 8 <= bodyLen) {
                    off += 4; // reference_ID
                    const uint32_t timescale = readU32BE(body + off);
                    off += 4;
                    uint64_t earliestPts = 0;
                    uint64_t firstOffset = 0;
                    if (version == 0) {
                        if (off + 8 <= bodyLen) {
                            earliestPts = readU32BE(body + off);
                            firstOffset = readU32BE(body + off + 4);
                            off += 8;
                        }
                    } else {
                        if (off + 16 <= bodyLen) {
                            earliestPts = readU64BE(body + off);
                            firstOffset = readU64BE(body + off + 8);
                            off += 16;
                        }
                    }
                    if (timescale > 0 && off + 4 <= bodyLen) {
                        off += 2; // reserved
                        const uint16_t refCount = readU16BE(body + off);
                        off += 2;

                        const size_t availEntries = (bodyLen - off) / 12;
                        const size_t count = std::min<size_t>(refCount, availEntries);
                        res.entries.reserve(res.entries.size() + count);

                        uint64_t curOffset = pos + boxSize + firstOffset;
                        if (res.firstMediaOffset == 0) {
                            res.firstMediaOffset = curOffset;
                        }
                        double curTime = static_cast<double>(earliestPts) / static_cast<double>(timescale);

                        for (size_t i = 0; i < count; ++i) {
                            const uint32_t w1 = readU32BE(body + off);
                            const uint32_t dur = readU32BE(body + off + 4);
                            const uint32_t w3 = readU32BE(body + off + 8);
                            off += 12;

                            const uint32_t refType = (w1 >> 31) & 1U;
                            const uint32_t refSize = w1 & 0x7FFFFFFFU;
                            const bool startsWithSap = ((w3 >> 31) & 1U) != 0;
                            const double durSec = static_cast<double>(dur) / static_cast<double>(timescale);

                            if (refType == 0 && refSize > 0 && curOffset + refSize <= totalFileSize) {
                                SidxEntry entry;
                                entry.offset = curOffset;
                                entry.size = refSize;
                                entry.startSec = curTime;
                                entry.durationSec = durSec;
                                entry.startsWithSap = startsWithSap;
                                res.entries.push_back(entry);
                            }
                            curOffset += refSize;
                            curTime += durSec;
                        }
                        res.totalDurationSec = std::max(res.totalDurationSec, curTime);
                        sawSidx = !res.entries.empty();
                    }
                }
            }
        } else if (tagEq(typePtr, "moof")) {
            sawMoof = true;
            if (sawSidx) {
                break; // 已经解析完头部 sidx，后面全是媒体片段，直接结束扫描
            }
        } else if (tagEq(typePtr, "mdat")) {
            // 遇到普通 MP4（moov 在尾部的 mdat）或流媒体主体，停止头部解析
            break;
        }

        pos += static_cast<size_t>(boxSize);
    }

    if (sawFtyp && sawMoov) {
        res.valid = true;
        res.isFragmented = sawSidx && !res.entries.empty();
        if (res.firstMediaOffset == 0 && sawMoof) {
            res.firstMediaOffset = pos;
        }
    } else if (sawFtyp) {
        // 可能是 moov 在末尾的普通 MP4（非 fMP4），标记 valid=true 但 isFragmented=false 走标准 FFmpeg 路径
        res.valid = true;
        res.isFragmented = false;
    }

    return res;
}

bool SidxParser::probeMediaInfo(const QByteArray& initPlusFrag,
                                QString& outCodec,
                                int& outWidth,
                                int& outHeight,
                                double& outAvgFps) {
    if (initPlusFrag.size() < 64) return false;

    constexpr int kIoBufSize = 32768;
    auto* avioBuf = static_cast<uint8_t*>(av_malloc(kIoBufSize));
    if (!avioBuf) return false;

    MemBufferCursor cursor{
        reinterpret_cast<const uint8_t*>(initPlusFrag.constData()),
        static_cast<size_t>(initPlusFrag.size()),
        0
    };

    AVIOContext* avio = avio_alloc_context(
        avioBuf, kIoBufSize, 0, &cursor, &memReadPacket, nullptr, &memSeekPacket);
    if (!avio) {
        av_free(avioBuf);
        return false;
    }

    AVFormatContext* fmt = avformat_alloc_context();
    if (!fmt) {
        av_freep(&avio->buffer);
        avio_context_free(&avio);
        return false;
    }
    fmt->pb = avio;
    fmt->flags |= AVFMT_FLAG_CUSTOM_IO;

    bool ok = false;
    if (avformat_open_input(&fmt, nullptr, nullptr, nullptr) >= 0) {
        if (avformat_find_stream_info(fmt, nullptr) >= 0) {
            for (unsigned i = 0; i < fmt->nb_streams; ++i) {
                const AVStream* st = fmt->streams[i];
                if (st->codecpar && st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                    const char* cname = avcodec_get_name(st->codecpar->codec_id);
                    outCodec = cname ? QString::fromLatin1(cname) : QStringLiteral("unknown");
                    outWidth = st->codecpar->width;
                    outHeight = st->codecpar->height;
                    if (st->avg_frame_rate.num > 0 && st->avg_frame_rate.den > 0) {
                        outAvgFps = av_q2d(st->avg_frame_rate);
                    } else if (st->r_frame_rate.num > 0 && st->r_frame_rate.den > 0) {
                        outAvgFps = av_q2d(st->r_frame_rate);
                    }
                    ok = (outWidth > 0 && outHeight > 0);
                    break;
                }
            }
        }
        avformat_close_input(&fmt);
    } else {
        avformat_free_context(fmt);
    }

    if (avio) {
        av_freep(&avio->buffer);
        avio_context_free(&avio);
    }
    return ok;
}

} // namespace mcs
