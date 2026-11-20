#pragma once

#include "core/Types.h"

#include <QString>
#include <cstdint>

namespace mcs {

struct OcrResult {
    bool ok = false;
    int64_t detectedWallMs = 0;
    QString rawDigits;        // "YYYYMMDDHHMMSS"
    QString formattedText;    // "YYYY-MM-DD HH:MM:SS"
    double avgConfidence = 0.0;
    double minConfidence = 0.0;
};

// 针对小米监控左上角白字黑描边 OSD 时间戳的跨平台零依赖极速 OCR
class TimestampOcr {
public:
    static OcrResult recognizeFromYPlane(const uint8_t* yPlane,
                                         int width,
                                         int height,
                                         int yStride,
                                         int tzOffsetSec);

    static OcrResult recognizeFromFrame(const DecodedFrame& frame, int tzOffsetSec);
};

} // namespace mcs
