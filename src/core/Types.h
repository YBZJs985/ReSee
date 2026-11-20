#pragma once

#include <QMetaType>
#include <QString>
#include <QVector>
#include <cstdint>
#include <memory>
#include <vector>

namespace mcs {

enum class DecodeState : int {
    Idle = 0,
    Buffering = 1,
    Playing = 2,
    Preview = 3,
    NoVideo = 4,
    Corrupt = 5,
    Disconnected = 6,
    Locating = 7,
    Indexing = 8
};

inline QString decodeStateLabel(DecodeState s) {
    switch (s) {
    case DecodeState::Idle: return QStringLiteral("空闲");
    case DecodeState::Buffering: return QStringLiteral("缓冲中");
    case DecodeState::Playing: return QStringLiteral("已同步");
    case DecodeState::Preview: return QStringLiteral("穿梭预览");
    case DecodeState::NoVideo: return QStringLiteral("无录像");
    case DecodeState::Corrupt: return QStringLiteral("读取失败");
    case DecodeState::Disconnected: return QStringLiteral("读取失败");
    case DecodeState::Locating: return QStringLiteral("定位中");
    case DecodeState::Indexing: return QStringLiteral("索引中");
    }
    return QStringLiteral("未知");
}

enum class SegmentStatus : int {
    Ok = 0,
    Writing = 1,
    Corrupt = 2,
    Missing = 3
};

struct CameraConfig {
    QString cameraId;
    QString displayName;
    QString rootPath;
    bool isDual = false;
    int sortOrder = 0;
    int onlineMaxMin = 30;
    int offlineMaxMin = 60;
    bool recursive = true;
    int maxDepth = 3;
};

struct StreamConfig {
    int streamIndex = 0;
    QString streamKey;      // e.g. "CAM001:00"
    QString cameraId;       // e.g. "CAM001"
    QString channel;        // "00" or "10"
    QString displayName;    // e.g. "双摄 · 通道00"
    QString rootPath;
    int64_t defaultOffsetMs = 0; // 流级时间校准偏移 (ms)
};

struct SegmentRecord {
    int64_t id = 0;
    int streamIndex = 0;
    QString streamKey;
    QString path;
    int64_t nominalStartMs = 0;
    int64_t nominalEndMs = 0;
    int64_t realDurationMs = 0;
    int64_t fileSize = 0;
    int64_t mtimeMs = 0;
    QString codec;
    int width = 0;
    int height = 0;
    double avgFps = 0.0;
    int64_t segmentOffsetMs = 0; // 片段级覆盖偏移（相对流级额外微调）
    double calibConfidence = 0.0;
    SegmentStatus status = SegmentStatus::Ok;
    bool isFragmented = true;

    int64_t effectiveEndMs() const {
        if (realDurationMs > 0) {
            const int64_t realEnd = nominalStartMs + realDurationMs;
            // 若容器实测时长仅比文件名名义结束时间短 <= 5 秒（常见封装尾差），以两者较大值为覆盖边界，避免切片结尾出现假空洞
            if (nominalEndMs > realEnd && (nominalEndMs - realEnd) <= 5000) {
                return nominalEndMs;
            }
            return realEnd;
        }
        return nominalEndMs;
    }
};

struct TimeInterval {
    int64_t startMs = 0;
    int64_t endMs = 0;
    SegmentStatus status = SegmentStatus::Ok;
};

struct DecodedFrame {
    int streamIndex = -1;
    int64_t ptsUs = 0;          // 文件内时间微秒（含 sidx 偏移）
    int64_t wallTimeMs = 0;     // 校准后的真实时间 (Unix ms)
    int64_t nominalTimeMs = 0;  // 未校准名义时间 (Unix ms)
    int width = 0;
    int height = 0;
    int yStride = 0;
    int uvStride = 0;
    // 连续存放 Y (height * yStride) + UV (height/2 * uvStride) 的 NV12 平面数据
    std::shared_ptr<std::vector<uint8_t>> nv12Data;
    bool isPreview = false;
    bool isHardware = false;
    uint64_t seekGen = 0;
    QString segmentPath;
};

struct StreamDiagnostics {
    int streamIndex = 0;
    QString streamKey;
    QString displayName;
    DecodeState state = DecodeState::Idle;
    int64_t currentWallTimeMs = 0;
    int64_t syncErrorMs = 0;      // currentWallTimeMs - masterTimeMs
    double decodedFps = 0.0;
    int queueDepth = 0;
    bool hwAccelActive = false;
    QString hwBackend;            // "D3D11VA", "VideoToolbox", "SW"
    int64_t appliedOffsetMs = 0;
    double calibConfidence = 0.0;
    double lastSeekMs = 0.0;
    double lastNasReadMs = 0.0;
    int64_t waitDurationMs = 0;
    bool hasValidTime = false;
    bool hasAudioTrack = false;
    int width = 0;
    int height = 0;
    QString currentFile;
    QString errorMessage;
};

struct BookmarkItem {
    int64_t id = 0;
    int64_t timestampMs = 0;
    QString title;
    QString note;
    QString color = QStringLiteral("#3B82F6");
    int streamIndex = -1; // -1 表示全局
    QString screenshotPath;
    int64_t createdAtMs = 0;
};

struct ScanProgress {
    bool scanning = false;
    bool stageBRunning = false;
    int totalFiles = 0;
    int probedFiles = 0;
    QString statusText;
};

} // namespace mcs

Q_DECLARE_METATYPE(mcs::DecodeState)
Q_DECLARE_METATYPE(mcs::StreamDiagnostics)
Q_DECLARE_METATYPE(mcs::BookmarkItem)
Q_DECLARE_METATYPE(mcs::ScanProgress)
