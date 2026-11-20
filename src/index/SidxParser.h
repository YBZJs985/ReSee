#pragma once

#include <QByteArray>
#include <QString>
#include <cstdint>
#include <vector>

namespace mcs {

struct SidxEntry {
    uint64_t offset = 0;      // 文件内绝对字节偏移
    uint32_t size = 0;        // fragment (moof + mdat) 字节数
    double startSec = 0.0;    // 该 fragment 起始时间（秒，相对文件头）
    double durationSec = 0.0; // 该 fragment 时长（秒）
    bool startsWithSap = true;
};

struct SidxIndex {
    bool valid = false;        // 文件头部是否合法（非损坏）
    bool isFragmented = false; // 是否包含 ftyp + moov + sidx（可使用极速直读）
    bool needMoreHeader = false;
    uint64_t suggestedHeaderBytes = 0;

    QByteArray initSegment;    // ftyp + moov 字节序列
    std::vector<SidxEntry> entries;
    double totalDurationSec = 0.0;
    uint64_t firstMediaOffset = 0;

    // 从 initSegment + firstFragment 提取（可选）
    QString codec;
    int width = 0;
    int height = 0;
    double avgFps = 0.0;

    // O(log N) 查找包含 targetSec 的 fragment 下标；若超出范围返回边界或 -1
    int findEntryAt(double targetSec) const;
};

class SidxParser {
public:
    // 从已读取的文件头部字节解析 ftyp/moov/sidx；totalFileSize 用于边界校验
    static SidxIndex parseHeaderBytes(const uint8_t* data, size_t len, uint64_t totalFileSize);

    // 可选：用内存中的 [initSegment + firstFrag] 通过 FFmpeg 快速探测分辨率、编码与帧率
    static bool probeMediaInfo(const QByteArray& initPlusFrag,
                               QString& outCodec,
                               int& outWidth,
                               int& outHeight,
                               double& outAvgFps);
};

} // namespace mcs
