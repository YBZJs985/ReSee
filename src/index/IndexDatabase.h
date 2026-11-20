#pragma once

#include "core/Types.h"

#include <QHash>
#include <QMutex>
#include <QPair>
#include <QString>
#include <QVector>
#include <vector>

struct sqlite3;

namespace mcs {

struct ExistingFileMeta {
    QString streamKey;
    int64_t id = 0;
    int64_t fileSize = 0;
    int64_t mtimeMs = 0;
    int64_t realDurationMs = 0;
    SegmentStatus status = SegmentStatus::Ok;
};

class IndexDatabase {
public:
    IndexDatabase() = default;
    ~IndexDatabase();

    IndexDatabase(const IndexDatabase&) = delete;
    IndexDatabase& operator=(const IndexDatabase&) = delete;

    bool open(const QString& dbPath);
    void close();
    bool isOpen() const;

    bool syncStreamsConfig(const QVector<CameraConfig>& cameras, const QVector<StreamConfig>& streams);

    // 用于快速增量扫描：一次性取出已索引文件的路径和元数据
    QHash<QString, ExistingFileMeta> loadExistingFilesMeta();
    QHash<QString, ExistingFileMeta> loadExistingFilesMetaForStreams(const QVector<int>& streamIndices);

    bool insertOrUpdateSegmentsBatch(const QVector<SegmentRecord>& records, int64_t scanGen);
    bool touchScanGenBatch(const QVector<int64_t>& ids, int64_t scanGen);
    int deleteMissingSegments(int64_t activeScanGen);
    int deleteMissingSegmentsForStreams(const QVector<int>& streamIndices, int64_t activeScanGen);

    struct StreamStorageStats {
        int64_t totalBytes = 0;
        int64_t fileCount = 0;
    };
    QHash<int, StreamStorageStats> loadStreamStorageStats();

    struct ProbeUpdate {
        int64_t id = 0;
        int64_t realDurationMs = 0;
        QString codec;
        int width = 0;
        int height = 0;
        double avgFps = 0.0;
        SegmentStatus status = SegmentStatus::Ok;
        bool isFragmented = true;
    };
    bool updateSegmentProbeBatch(const QVector<ProbeUpdate>& updates);

    // 读取待探测切片（real_duration_ms == 0 && status == Ok）
    QVector<SegmentRecord> loadPendingProbeSegments(int limit = 2000);

    // 加载每个通道按时间排序的全量切片，供播放器核心做 O(log N) 内存二分查找
    std::vector<std::vector<SegmentRecord>> loadAllSegmentsByStream(int streamCount);

    // 校准管理
    bool updateSegmentCalibration(int64_t segmentId, int64_t offsetMs, double confidence,
                                  const QString& source, const QString& note);
    bool updateStreamOffset(int streamIndex, int64_t offsetMs);
    QHash<int, int64_t> loadStreamOffsets();

    // 书签/事件标记 CRUD
    int64_t addBookmark(const BookmarkItem& item);
    bool updateBookmark(const BookmarkItem& item);
    bool deleteBookmark(int64_t id);
    QVector<BookmarkItem> listBookmarks(const QString& queryText = QString(),
                                        int64_t fromMs = 0,
                                        int64_t toMs = 0);

    // 键值状态（断点续看等）
    bool setKv(const QString& key, const QString& value);
    QString getKv(const QString& key, const QString& defaultVal = QString());

private:
    bool createSchema();
    bool execSql(const char* sql);

    mutable QMutex m_mutex;
    sqlite3* m_db = nullptr;
};

} // namespace mcs
