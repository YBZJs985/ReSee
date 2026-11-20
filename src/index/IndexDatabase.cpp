#include "index/IndexDatabase.h"
#include "core/Logger.h"
#include "core/TimeUtils.h"

#include <QDir>
#include <QFileInfo>
#include <sqlite3.h>
#include <algorithm>

namespace mcs {

namespace {
QString colText(sqlite3_stmt* stmt, int col) {
    const unsigned char* txt = sqlite3_column_text(stmt, col);
    return txt ? QString::fromUtf8(reinterpret_cast<const char*>(txt)) : QString();
}
} // namespace

IndexDatabase::~IndexDatabase() {
    close();
}

bool IndexDatabase::open(const QString& dbPath) {
    QMutexLocker locker(&m_mutex);
    if (m_db) {
        sqlite3_close_v2(m_db);
        m_db = nullptr;
    }

    QDir().mkpath(QFileInfo(dbPath).absolutePath());
    const QByteArray utf8Path = dbPath.toUtf8();
    if (sqlite3_open_v2(utf8Path.constData(), &m_db,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
        MCS_LOG_ERROR("DB", QStringLiteral("无法打开索引数据库: %1 (%2)")
                                .arg(dbPath, QString::fromUtf8(sqlite3_errmsg(m_db))));
        if (m_db) {
            sqlite3_close_v2(m_db);
            m_db = nullptr;
        }
        return false;
    }

    execSql("PRAGMA journal_mode=WAL;");
    execSql("PRAGMA synchronous=NORMAL;");
    execSql("PRAGMA temp_store=MEMORY;");
    execSql("PRAGMA cache_size=-32768;");
    execSql("PRAGMA busy_timeout=5000;");

    return createSchema();
}

void IndexDatabase::close() {
    QMutexLocker locker(&m_mutex);
    if (m_db) {
        sqlite3_close_v2(m_db);
        m_db = nullptr;
    }
}

bool IndexDatabase::isOpen() const {
    QMutexLocker locker(&m_mutex);
    return m_db != nullptr;
}

bool IndexDatabase::execSql(const char* sql) {
    if (!m_db) return false;
    char* err = nullptr;
    const int rc = sqlite3_exec(m_db, sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        MCS_LOG_ERROR("DB", QStringLiteral("SQL 执行失败: %1").arg(err ? QString::fromUtf8(err) : QString()));
        sqlite3_free(err);
        return false;
    }
    return true;
}

bool IndexDatabase::createSchema() {
    const char* ddl = R"SQL(
        CREATE TABLE IF NOT EXISTS cameras (
            id TEXT PRIMARY KEY,
            display_name TEXT NOT NULL,
            root_path TEXT NOT NULL,
            is_dual INTEGER NOT NULL DEFAULT 0,
            sort_order INTEGER NOT NULL DEFAULT 0
        );
        CREATE TABLE IF NOT EXISTS streams (
            stream_index INTEGER PRIMARY KEY,
            stream_key TEXT NOT NULL UNIQUE,
            camera_id TEXT NOT NULL,
            channel TEXT NOT NULL,
            display_name TEXT NOT NULL,
            offset_ms INTEGER NOT NULL DEFAULT 0
        );
        CREATE TABLE IF NOT EXISTS segments (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            stream_index INTEGER NOT NULL,
            stream_key TEXT NOT NULL,
            path TEXT NOT NULL UNIQUE,
            nominal_start_ms INTEGER NOT NULL,
            nominal_end_ms INTEGER NOT NULL,
            real_duration_ms INTEGER NOT NULL DEFAULT 0,
            file_size INTEGER NOT NULL DEFAULT 0,
            mtime_ms INTEGER NOT NULL DEFAULT 0,
            codec TEXT NOT NULL DEFAULT '',
            width INTEGER NOT NULL DEFAULT 0,
            height INTEGER NOT NULL DEFAULT 0,
            avg_fps REAL NOT NULL DEFAULT 0.0,
            segment_offset_ms INTEGER NOT NULL DEFAULT 0,
            calib_confidence REAL NOT NULL DEFAULT 0.0,
            status INTEGER NOT NULL DEFAULT 0,
            is_fragmented INTEGER NOT NULL DEFAULT 1,
            scan_gen INTEGER NOT NULL DEFAULT 0
        );
        CREATE INDEX IF NOT EXISTS idx_seg_stream_time
            ON segments(stream_index, nominal_start_ms, nominal_end_ms);
        CREATE INDEX IF NOT EXISTS idx_seg_gen
            ON segments(scan_gen);

        CREATE TABLE IF NOT EXISTS calibrations (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            stream_index INTEGER NOT NULL,
            segment_id INTEGER NOT NULL DEFAULT 0,
            offset_ms INTEGER NOT NULL DEFAULT 0,
            source TEXT NOT NULL DEFAULT '',
            confidence REAL NOT NULL DEFAULT 0.0,
            verified_at_ms INTEGER NOT NULL DEFAULT 0,
            note TEXT NOT NULL DEFAULT ''
        );

        CREATE TABLE IF NOT EXISTS bookmarks (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp_ms INTEGER NOT NULL,
            title TEXT NOT NULL,
            note TEXT NOT NULL DEFAULT '',
            color TEXT NOT NULL DEFAULT '#3B82F6',
            stream_index INTEGER NOT NULL DEFAULT -1,
            screenshot_path TEXT NOT NULL DEFAULT '',
            created_at_ms INTEGER NOT NULL DEFAULT 0
        );
        CREATE INDEX IF NOT EXISTS idx_bm_time ON bookmarks(timestamp_ms);

        CREATE TABLE IF NOT EXISTS kv_state (
            key TEXT PRIMARY KEY,
            value TEXT NOT NULL
        );
    )SQL";
    return execSql(ddl);
}

bool IndexDatabase::syncStreamsConfig(const QVector<CameraConfig>& cameras, const QVector<StreamConfig>& streams) {
    QMutexLocker locker(&m_mutex);
    if (!m_db) return false;
    if (!execSql("BEGIN IMMEDIATE;")) return false;

    bool ok = execSql("DELETE FROM cameras;");
    sqlite3_stmt* stmtCam = nullptr;
    if (ok && sqlite3_prepare_v2(m_db,
            "INSERT INTO cameras(id, display_name, root_path, is_dual, sort_order) VALUES(?1,?2,?3,?4,?5);",
            -1, &stmtCam, nullptr) == SQLITE_OK) {
        for (const auto& c : cameras) {
            sqlite3_reset(stmtCam);
            sqlite3_bind_text(stmtCam, 1, c.cameraId.toUtf8().constData(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmtCam, 2, c.displayName.toUtf8().constData(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmtCam, 3, c.rootPath.toUtf8().constData(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmtCam, 4, c.isDual ? 1 : 0);
            sqlite3_bind_int(stmtCam, 5, c.sortOrder);
            if (sqlite3_step(stmtCam) != SQLITE_DONE) {
                ok = false;
                break;
            }
        }
        sqlite3_finalize(stmtCam);
    } else {
        ok = false;
    }

    // 保留已有流级偏移（如有）
    QHash<QString, int64_t> existingOffsets;
    if (ok) {
        sqlite3_stmt* stmtLoad = nullptr;
        if (sqlite3_prepare_v2(m_db, "SELECT stream_key, offset_ms FROM streams;", -1, &stmtLoad, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmtLoad) == SQLITE_ROW) {
                existingOffsets.insert(colText(stmtLoad, 0), sqlite3_column_int64(stmtLoad, 1));
            }
            sqlite3_finalize(stmtLoad);
        } else {
            ok = false;
        }
    }

    // Keep the old stream identities available until bookmarks and segments are remapped.
    if (ok) {
        ok = execSql("CREATE TEMP TABLE previous_streams AS SELECT stream_index, stream_key FROM streams;")
            && execSql("DELETE FROM streams;");
    }
    sqlite3_stmt* stmtSt = nullptr;
    if (ok && sqlite3_prepare_v2(m_db,
            "INSERT INTO streams(stream_index, stream_key, camera_id, channel, display_name, offset_ms) VALUES(?1,?2,?3,?4,?5,?6);",
            -1, &stmtSt, nullptr) == SQLITE_OK) {
        for (const auto& s : streams) {
            const int64_t off = existingOffsets.contains(s.streamKey)
                ? existingOffsets.value(s.streamKey)
                : s.defaultOffsetMs;
            sqlite3_reset(stmtSt);
            sqlite3_bind_int(stmtSt, 1, s.streamIndex);
            sqlite3_bind_text(stmtSt, 2, s.streamKey.toUtf8().constData(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmtSt, 3, s.cameraId.toUtf8().constData(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmtSt, 4, s.channel.toUtf8().constData(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmtSt, 5, s.displayName.toUtf8().constData(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(stmtSt, 6, off);
            if (sqlite3_step(stmtSt) != SQLITE_DONE) {
                ok = false;
                break;
            }
        }
        sqlite3_finalize(stmtSt);
    } else {
        ok = false;
    }

    if (ok) {
        // A camera removal can move all later streams to different indices.
        ok = execSql("UPDATE bookmarks SET stream_index = COALESCE((SELECT streams.stream_index "
                     "FROM previous_streams JOIN streams USING (stream_key) "
                     "WHERE previous_streams.stream_index = bookmarks.stream_index), -1) "
                     "WHERE stream_index IN (SELECT old.stream_index FROM previous_streams old "
                     "LEFT JOIN streams USING (stream_key) "
                     "WHERE streams.stream_index IS NULL OR streams.stream_index != old.stream_index);")
            && execSql("DELETE FROM segments WHERE stream_key NOT IN (SELECT stream_key FROM streams);")
            && execSql("UPDATE segments SET stream_index = (SELECT stream_index FROM streams "
                       "WHERE streams.stream_key = segments.stream_key) "
                       "WHERE stream_index != (SELECT stream_index FROM streams "
                       "WHERE streams.stream_key = segments.stream_key);")
            && execSql("DROP TABLE previous_streams;");
    }

    if (!ok || !execSql("COMMIT;")) {
        execSql("ROLLBACK;");
        return false;
    }
    return true;
}

QHash<QString, ExistingFileMeta> IndexDatabase::loadExistingFilesMeta() {
    QMutexLocker locker(&m_mutex);
    QHash<QString, ExistingFileMeta> result;
    if (!m_db) return result;

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db,
            "SELECT path, id, file_size, mtime_ms, real_duration_ms, status, stream_key FROM segments;",
            -1, &stmt, nullptr) != SQLITE_OK) {
        return result;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ExistingFileMeta meta;
        const QString p = colText(stmt, 0);
        meta.id = sqlite3_column_int64(stmt, 1);
        meta.fileSize = sqlite3_column_int64(stmt, 2);
        meta.mtimeMs = sqlite3_column_int64(stmt, 3);
        meta.realDurationMs = sqlite3_column_int64(stmt, 4);
        meta.status = static_cast<SegmentStatus>(sqlite3_column_int(stmt, 5));
        meta.streamKey = colText(stmt, 6);
        result.insert(p, meta);
    }
    sqlite3_finalize(stmt);
    return result;
}

bool IndexDatabase::insertOrUpdateSegmentsBatch(const QVector<SegmentRecord>& records, int64_t scanGen) {
    if (records.isEmpty()) return true;
    QMutexLocker locker(&m_mutex);
    if (!m_db) return false;

    if (!execSql("BEGIN IMMEDIATE;")) return false;
    const char* sql = R"SQL(
        INSERT INTO segments(
            stream_index, stream_key, path, nominal_start_ms, nominal_end_ms,
            real_duration_ms, file_size, mtime_ms, codec, width, height, avg_fps,
            segment_offset_ms, calib_confidence, status, is_fragmented, scan_gen
        ) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17)
        ON CONFLICT(path) DO UPDATE SET
            stream_index = excluded.stream_index,
            stream_key = excluded.stream_key,
            nominal_start_ms = excluded.nominal_start_ms,
            nominal_end_ms = excluded.nominal_end_ms,
            file_size = excluded.file_size,
            mtime_ms = excluded.mtime_ms,
            status = excluded.status,
            real_duration_ms = CASE WHEN segments.file_size != excluded.file_size THEN 0 ELSE segments.real_duration_ms END,
            scan_gen = excluded.scan_gen;
    )SQL";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        execSql("ROLLBACK;");
        return false;
    }

    bool ok = true;
    for (const auto& r : records) {
        sqlite3_reset(stmt);
        sqlite3_bind_int(stmt, 1, r.streamIndex);
        sqlite3_bind_text(stmt, 2, r.streamKey.toUtf8().constData(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, r.path.toUtf8().constData(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 4, r.nominalStartMs);
        sqlite3_bind_int64(stmt, 5, r.nominalEndMs);
        sqlite3_bind_int64(stmt, 6, r.realDurationMs);
        sqlite3_bind_int64(stmt, 7, r.fileSize);
        sqlite3_bind_int64(stmt, 8, r.mtimeMs);
        sqlite3_bind_text(stmt, 9, r.codec.toUtf8().constData(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 10, r.width);
        sqlite3_bind_int(stmt, 11, r.height);
        sqlite3_bind_double(stmt, 12, r.avgFps);
        sqlite3_bind_int64(stmt, 13, r.segmentOffsetMs);
        sqlite3_bind_double(stmt, 14, r.calibConfidence);
        sqlite3_bind_int(stmt, 15, static_cast<int>(r.status));
        sqlite3_bind_int(stmt, 16, r.isFragmented ? 1 : 0);
        sqlite3_bind_int64(stmt, 17, scanGen);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            ok = false;
            break;
        }
    }

    sqlite3_finalize(stmt);
    if (!ok || !execSql("COMMIT;")) {
        execSql("ROLLBACK;");
        return false;
    }
    return true;
}

bool IndexDatabase::touchScanGenBatch(const QVector<int64_t>& ids, int64_t scanGen) {
    if (ids.isEmpty()) return true;
    QMutexLocker locker(&m_mutex);
    if (!m_db) return false;

    if (!execSql("BEGIN IMMEDIATE;")) return false;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, "UPDATE segments SET scan_gen = ?1 WHERE id = ?2;", -1, &stmt, nullptr) != SQLITE_OK) {
        execSql("ROLLBACK;");
        return false;
    }

    bool ok = true;
    for (const int64_t id : ids) {
        sqlite3_reset(stmt);
        sqlite3_bind_int64(stmt, 1, scanGen);
        sqlite3_bind_int64(stmt, 2, id);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            ok = false;
            break;
        }
    }
    sqlite3_finalize(stmt);
    if (!ok || !execSql("COMMIT;")) {
        execSql("ROLLBACK;");
        return false;
    }
    return true;
}

int IndexDatabase::deleteMissingSegments(int64_t activeScanGen) {
    QMutexLocker locker(&m_mutex);
    if (!m_db) return 0;

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, "DELETE FROM segments WHERE scan_gen != ?1;", -1, &stmt, nullptr) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_int64(stmt, 1, activeScanGen);
    sqlite3_step(stmt);
    const int changes = sqlite3_changes(m_db);
    sqlite3_finalize(stmt);
    return changes;
}

QHash<QString, ExistingFileMeta> IndexDatabase::loadExistingFilesMetaForStreams(const QVector<int>& streamIndices) {
    QMutexLocker locker(&m_mutex);
    QHash<QString, ExistingFileMeta> result;
    if (!m_db || streamIndices.isEmpty()) return result;

    QStringList placeholders;
    for (int i = 0; i < streamIndices.size(); ++i) {
        placeholders.append(QStringLiteral("?%1").arg(i + 1));
    }
    const QString sql = QStringLiteral(
        "SELECT path, id, file_size, mtime_ms, real_duration_ms, status FROM segments WHERE stream_index IN (%1);")
                            .arg(placeholders.join(QLatin1Char(',')));

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql.toUtf8().constData(), -1, &stmt, nullptr) != SQLITE_OK) {
        return result;
    }
    for (int i = 0; i < streamIndices.size(); ++i) {
        sqlite3_bind_int(stmt, i + 1, streamIndices.at(i));
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ExistingFileMeta meta;
        const QString p = colText(stmt, 0);
        meta.id = sqlite3_column_int64(stmt, 1);
        meta.fileSize = sqlite3_column_int64(stmt, 2);
        meta.mtimeMs = sqlite3_column_int64(stmt, 3);
        meta.realDurationMs = sqlite3_column_int64(stmt, 4);
        meta.status = static_cast<SegmentStatus>(sqlite3_column_int(stmt, 5));
        result.insert(p, meta);
    }
    sqlite3_finalize(stmt);
    return result;
}

int IndexDatabase::deleteMissingSegmentsForStreams(const QVector<int>& streamIndices, int64_t activeScanGen) {
    QMutexLocker locker(&m_mutex);
    if (!m_db || streamIndices.isEmpty()) return 0;

    QStringList placeholders;
    for (int i = 0; i < streamIndices.size(); ++i) {
        placeholders.append(QStringLiteral("?%1").arg(i + 2));
    }
    const QString sql = QStringLiteral("DELETE FROM segments WHERE scan_gen != ?1 AND stream_index IN (%1);")
                            .arg(placeholders.join(QLatin1Char(',')));

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql.toUtf8().constData(), -1, &stmt, nullptr) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_int64(stmt, 1, activeScanGen);
    for (int i = 0; i < streamIndices.size(); ++i) {
        sqlite3_bind_int(stmt, i + 2, streamIndices.at(i));
    }
    sqlite3_step(stmt);
    const int changes = sqlite3_changes(m_db);
    sqlite3_finalize(stmt);
    return changes;
}

QHash<int, IndexDatabase::StreamStorageStats> IndexDatabase::loadStreamStorageStats() {
    QMutexLocker locker(&m_mutex);
    QHash<int, StreamStorageStats> out;
    if (!m_db) return out;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db,
            "SELECT stream_index, COALESCE(SUM(file_size), 0), COUNT(*) FROM segments GROUP BY stream_index;",
            -1, &stmt, nullptr) != SQLITE_OK) {
        return out;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const int idx = sqlite3_column_int(stmt, 0);
        StreamStorageStats st;
        st.totalBytes = sqlite3_column_int64(stmt, 1);
        st.fileCount = sqlite3_column_int64(stmt, 2);
        out.insert(idx, st);
    }
    sqlite3_finalize(stmt);
    return out;
}

bool IndexDatabase::updateSegmentProbeBatch(const QVector<ProbeUpdate>& updates) {
    if (updates.isEmpty()) return true;
    QMutexLocker locker(&m_mutex);
    if (!m_db) return false;

    if (!execSql("BEGIN IMMEDIATE;")) return false;
    const char* sql = R"SQL(
        UPDATE segments SET
            real_duration_ms = ?1,
            codec = CASE WHEN ?2 != '' THEN ?2 ELSE codec END,
            width = CASE WHEN ?3 > 0 THEN ?3 ELSE width END,
            height = CASE WHEN ?4 > 0 THEN ?4 ELSE height END,
            avg_fps = CASE WHEN ?5 > 0 THEN ?5 ELSE avg_fps END,
            status = ?6,
            is_fragmented = ?7
        WHERE id = ?8;
    )SQL";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        execSql("ROLLBACK;");
        return false;
    }

    bool ok = true;
    for (const auto& u : updates) {
        sqlite3_reset(stmt);
        sqlite3_bind_int64(stmt, 1, u.realDurationMs);
        sqlite3_bind_text(stmt, 2, u.codec.toUtf8().constData(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 3, u.width);
        sqlite3_bind_int(stmt, 4, u.height);
        sqlite3_bind_double(stmt, 5, u.avgFps);
        sqlite3_bind_int(stmt, 6, static_cast<int>(u.status));
        sqlite3_bind_int(stmt, 7, u.isFragmented ? 1 : 0);
        sqlite3_bind_int64(stmt, 8, u.id);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            ok = false;
            break;
        }
    }
    sqlite3_finalize(stmt);
    if (!ok || !execSql("COMMIT;")) {
        execSql("ROLLBACK;");
        return false;
    }
    return true;
}

QVector<SegmentRecord> IndexDatabase::loadPendingProbeSegments(int limit) {
    QMutexLocker locker(&m_mutex);
    QVector<SegmentRecord> out;
    if (!m_db) return out;

    const char* sql = R"SQL(
        SELECT id, stream_index, stream_key, path, nominal_start_ms, nominal_end_ms,
               real_duration_ms, file_size, mtime_ms, status, is_fragmented
        FROM segments
        WHERE (real_duration_ms = 0 AND status = 0)
           OR (status = 1 AND mtime_ms < ?2)
        ORDER BY nominal_start_ms DESC
        LIMIT ?1;
    )SQL";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return out;
    }
    sqlite3_bind_int(stmt, 1, limit);
    sqlite3_bind_int64(stmt, 2, TimeUtils::nowEpochMs() - 20000);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        SegmentRecord r;
        r.id = sqlite3_column_int64(stmt, 0);
        r.streamIndex = sqlite3_column_int(stmt, 1);
        r.streamKey = colText(stmt, 2);
        r.path = colText(stmt, 3);
        r.nominalStartMs = sqlite3_column_int64(stmt, 4);
        r.nominalEndMs = sqlite3_column_int64(stmt, 5);
        r.realDurationMs = sqlite3_column_int64(stmt, 6);
        r.fileSize = sqlite3_column_int64(stmt, 7);
        r.mtimeMs = sqlite3_column_int64(stmt, 8);
        r.status = static_cast<SegmentStatus>(sqlite3_column_int(stmt, 9));
        r.isFragmented = sqlite3_column_int(stmt, 10) != 0;
        out.append(r);
    }
    sqlite3_finalize(stmt);
    return out;
}

std::vector<std::vector<SegmentRecord>> IndexDatabase::loadAllSegmentsByStream(int streamCount) {
    QMutexLocker locker(&m_mutex);
    std::vector<std::vector<SegmentRecord>> result(static_cast<size_t>(std::max(0, streamCount)));
    if (!m_db || streamCount <= 0) return result;

    const char* sql = R"SQL(
        SELECT id, stream_index, stream_key, path, nominal_start_ms, nominal_end_ms,
               real_duration_ms, file_size, mtime_ms, codec, width, height, avg_fps,
               segment_offset_ms, calib_confidence, status, is_fragmented
        FROM segments
        ORDER BY stream_index ASC, nominal_start_ms ASC;
    )SQL";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return result;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        SegmentRecord r;
        r.id = sqlite3_column_int64(stmt, 0);
        r.streamIndex = sqlite3_column_int(stmt, 1);
        r.streamKey = colText(stmt, 2);
        r.path = colText(stmt, 3);
        r.nominalStartMs = sqlite3_column_int64(stmt, 4);
        r.nominalEndMs = sqlite3_column_int64(stmt, 5);
        r.realDurationMs = sqlite3_column_int64(stmt, 6);
        r.fileSize = sqlite3_column_int64(stmt, 7);
        r.mtimeMs = sqlite3_column_int64(stmt, 8);
        r.codec = colText(stmt, 9);
        r.width = sqlite3_column_int(stmt, 10);
        r.height = sqlite3_column_int(stmt, 11);
        r.avgFps = sqlite3_column_double(stmt, 12);
        r.segmentOffsetMs = sqlite3_column_int64(stmt, 13);
        r.calibConfidence = sqlite3_column_double(stmt, 14);
        r.status = static_cast<SegmentStatus>(sqlite3_column_int(stmt, 15));
        r.isFragmented = sqlite3_column_int(stmt, 16) != 0;

        if (r.streamIndex >= 0 && r.streamIndex < static_cast<int>(result.size())) {
            result[static_cast<size_t>(r.streamIndex)].push_back(std::move(r));
        }
    }

    sqlite3_finalize(stmt);
    return result;
}

bool IndexDatabase::updateSegmentCalibration(int64_t segmentId, int64_t offsetMs, double confidence,
                                           const QString& source, const QString& note) {
    QMutexLocker locker(&m_mutex);
    if (!m_db) return false;
    if (!execSql("BEGIN IMMEDIATE;")) return false;

    bool ok = true;
    int streamIdx = 0;
    sqlite3_stmt* qStmt = nullptr;
    if (sqlite3_prepare_v2(m_db, "SELECT stream_index FROM segments WHERE id=?1;", -1, &qStmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(qStmt, 1, segmentId);
        if (sqlite3_step(qStmt) == SQLITE_ROW) {
            streamIdx = sqlite3_column_int(qStmt, 0);
        }
        sqlite3_finalize(qStmt);
    } else {
        ok = false;
    }

    sqlite3_stmt* uStmt = nullptr;
    if (ok && sqlite3_prepare_v2(m_db,
            "UPDATE segments SET segment_offset_ms=?1, calib_confidence=?2 WHERE id=?3;",
            -1, &uStmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(uStmt, 1, offsetMs);
        sqlite3_bind_double(uStmt, 2, confidence);
        sqlite3_bind_int64(uStmt, 3, segmentId);
        if (sqlite3_step(uStmt) != SQLITE_DONE) ok = false;
        sqlite3_finalize(uStmt);
    } else {
        ok = false;
    }

    sqlite3_stmt* iStmt = nullptr;
    if (ok && sqlite3_prepare_v2(m_db,
            "INSERT INTO calibrations(stream_index, segment_id, offset_ms, source, confidence, verified_at_ms, note) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7);",
            -1, &iStmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(iStmt, 1, streamIdx);
        sqlite3_bind_int64(iStmt, 2, segmentId);
        sqlite3_bind_int64(iStmt, 3, offsetMs);
        sqlite3_bind_text(iStmt, 4, source.toUtf8().constData(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(iStmt, 5, confidence);
        sqlite3_bind_int64(iStmt, 6, TimeUtils::nowEpochMs());
        sqlite3_bind_text(iStmt, 7, note.toUtf8().constData(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(iStmt) != SQLITE_DONE) ok = false;
        sqlite3_finalize(iStmt);
    } else {
        ok = false;
    }

    if (!ok || !execSql("COMMIT;")) {
        execSql("ROLLBACK;");
        return false;
    }
    return true;
}

bool IndexDatabase::updateStreamOffset(int streamIndex, int64_t offsetMs) {
    QMutexLocker locker(&m_mutex);
    if (!m_db) return false;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, "UPDATE streams SET offset_ms=?1 WHERE stream_index=?2;", -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_int64(stmt, 1, offsetMs);
    sqlite3_bind_int(stmt, 2, streamIndex);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

QHash<int, int64_t> IndexDatabase::loadStreamOffsets() {
    QMutexLocker locker(&m_mutex);
    QHash<int, int64_t> map;
    if (!m_db) return map;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, "SELECT stream_index, offset_ms FROM streams;", -1, &stmt, nullptr) != SQLITE_OK) {
        return map;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        map.insert(sqlite3_column_int(stmt, 0), sqlite3_column_int64(stmt, 1));
    }
    sqlite3_finalize(stmt);
    return map;
}

int64_t IndexDatabase::addBookmark(const BookmarkItem& item) {
    QMutexLocker locker(&m_mutex);
    if (!m_db) return 0;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db,
            "INSERT INTO bookmarks(timestamp_ms, title, note, color, stream_index, screenshot_path, created_at_ms) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7);",
            -1, &stmt, nullptr) != SQLITE_OK) {
        return 0;
    }
    const int64_t nowMs = item.createdAtMs > 0 ? item.createdAtMs : TimeUtils::nowEpochMs();
    sqlite3_bind_int64(stmt, 1, item.timestampMs);
    sqlite3_bind_text(stmt, 2, item.title.toUtf8().constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, item.note.toUtf8().constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, item.color.toUtf8().constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 5, item.streamIndex);
    sqlite3_bind_text(stmt, 6, item.screenshotPath.toUtf8().constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 7, nowMs);

    int64_t rowId = 0;
    if (sqlite3_step(stmt) == SQLITE_DONE) {
        rowId = sqlite3_last_insert_rowid(m_db);
    }
    sqlite3_finalize(stmt);
    return rowId;
}

bool IndexDatabase::updateBookmark(const BookmarkItem& item) {
    QMutexLocker locker(&m_mutex);
    if (!m_db) return false;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db,
            "UPDATE bookmarks SET timestamp_ms=?1, title=?2, note=?3, color=?4, stream_index=?5 WHERE id=?6;",
            -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_int64(stmt, 1, item.timestampMs);
    sqlite3_bind_text(stmt, 2, item.title.toUtf8().constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, item.note.toUtf8().constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, item.color.toUtf8().constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 5, item.streamIndex);
    sqlite3_bind_int64(stmt, 6, item.id);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

bool IndexDatabase::deleteBookmark(int64_t id) {
    QMutexLocker locker(&m_mutex);
    if (!m_db) return false;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, "DELETE FROM bookmarks WHERE id=?1;", -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_int64(stmt, 1, id);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

QVector<BookmarkItem> IndexDatabase::listBookmarks(const QString& queryText, int64_t fromMs, int64_t toMs) {
    QMutexLocker locker(&m_mutex);
    QVector<BookmarkItem> out;
    if (!m_db) return out;

    const QString trimmed = queryText.trimmed();
    QString sql = QStringLiteral(
        "SELECT id, timestamp_ms, title, note, color, stream_index, screenshot_path, created_at_ms "
        "FROM bookmarks WHERE 1=1");
    if (!trimmed.isEmpty()) {
        sql += QStringLiteral(" AND (title LIKE ?1 ESCAPE '\\' OR note LIKE ?1 ESCAPE '\\')");
    }
    if (fromMs > 0 && toMs > 0) {
        sql += QStringLiteral(" AND timestamp_ms >= ?2 AND timestamp_ms <= ?3");
    }
    sql += QStringLiteral(" ORDER BY timestamp_ms DESC;");

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql.toUtf8().constData(), -1, &stmt, nullptr) != SQLITE_OK) {
        return out;
    }
    if (!trimmed.isEmpty()) {
        QString escaped = trimmed;
        escaped.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
        escaped.replace(QLatin1Char('%'), QStringLiteral("\\%"));
        escaped.replace(QLatin1Char('_'), QStringLiteral("\\_"));
        const QString pat = QStringLiteral("%") + escaped + QStringLiteral("%");
        sqlite3_bind_text(stmt, 1, pat.toUtf8().constData(), -1, SQLITE_TRANSIENT);
    }
    if (fromMs > 0 && toMs > 0) {
        sqlite3_bind_int64(stmt, 2, fromMs);
        sqlite3_bind_int64(stmt, 3, toMs);
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        BookmarkItem b;
        b.id = sqlite3_column_int64(stmt, 0);
        b.timestampMs = sqlite3_column_int64(stmt, 1);
        b.title = colText(stmt, 2);
        b.note = colText(stmt, 3);
        b.color = colText(stmt, 4);
        b.streamIndex = sqlite3_column_int(stmt, 5);
        b.screenshotPath = colText(stmt, 6);
        b.createdAtMs = sqlite3_column_int64(stmt, 7);
        out.append(b);
    }
    sqlite3_finalize(stmt);
    return out;
}

bool IndexDatabase::setKv(const QString& key, const QString& value) {
    QMutexLocker locker(&m_mutex);
    if (!m_db) return false;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db,
            "INSERT INTO kv_state(key, value) VALUES(?1, ?2) "
            "ON CONFLICT(key) DO UPDATE SET value=excluded.value;",
            -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(stmt, 1, key.toUtf8().constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, value.toUtf8().constData(), -1, SQLITE_TRANSIENT);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

QString IndexDatabase::getKv(const QString& key, const QString& defaultVal) {
    QMutexLocker locker(&m_mutex);
    if (!m_db) return defaultVal;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, "SELECT value FROM kv_state WHERE key=?1;", -1, &stmt, nullptr) != SQLITE_OK) {
        return defaultVal;
    }
    sqlite3_bind_text(stmt, 1, key.toUtf8().constData(), -1, SQLITE_TRANSIENT);
    QString val = defaultVal;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        val = colText(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return val;
}

} // namespace mcs
