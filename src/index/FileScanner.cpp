#include "index/FileScanner.h"
#include "cache/NasBlockCache.h"
#include "core/Logger.h"
#include "core/TimeUtils.h"
#include "index/IndexDatabase.h"
#include "index/SidxParser.h"

#include <QDir>
#include <QHash>
#include <chrono>
#include <filesystem>

namespace mcs {

namespace {
#ifdef _WIN32
std::filesystem::path toFsPath(const QString& qpath) {
    return std::filesystem::path(qpath.toStdWString());
}
QString fromFsPath(const std::filesystem::path& p) {
    return QString::fromStdWString(p.generic_wstring());
}
#else
std::filesystem::path toFsPath(const QString& qpath) {
    return std::filesystem::path(qpath.toStdString());
}
QString fromFsPath(const std::filesystem::path& p) {
    return QString::fromStdString(p.generic_string());
}
#endif

int64_t fileTimeToEpochMs(std::filesystem::file_time_type ft) {
    const auto sysNow = std::chrono::system_clock::now();
    const auto fileNow = std::filesystem::file_time_type::clock::now();
    const auto sysTp = std::chrono::time_point_cast<std::chrono::milliseconds>(sysNow + (ft - fileNow));
    return sysTp.time_since_epoch().count();
}
} // namespace

FileScanner::FileScanner(IndexDatabase* db, NasBlockCache* cache, QObject* parent)
    : QObject(parent), m_db(db), m_cache(cache) {}

FileScanner::~FileScanner() {
    stop();
}

void FileScanner::stop() {
    m_stopRequested.store(true, std::memory_order_release);
    if (m_quickThread.joinable()) {
        m_quickThread.join();
    }
    if (m_thread.joinable()) {
        m_thread.join();
    }
    m_stageARunning.store(false, std::memory_order_release);
}

void FileScanner::startCameraRescan(const AppConfig& config, int streamIndex) {
    if (m_quickThread.joinable()) {
        m_quickThread.join();
    }
    m_quickThread = std::jthread([this, config, streamIndex]() {
        int changed = 0;
        doCameraQuickScan(config, streamIndex, changed);
        emit cameraRescanCompleted(streamIndex, changed);
    });
}

void FileScanner::setForegroundBusy(bool busy) {
    m_foregroundBusy.store(busy, std::memory_order_relaxed);
}

void FileScanner::startScan(const AppConfig& config, bool runStageB) {
    stop();
    m_stopRequested.store(false, std::memory_order_release);
    m_thread = std::jthread([this, config, runStageB]() {
        workerMain(config, runStageB);
    });
}

bool FileScanner::runQuickScanBlocking(const AppConfig& config, int& outTotalFiles) {
    m_stopRequested.store(false, std::memory_order_release);
    int changed = 0;
    int removed = 0;
    return doStageA(config, outTotalFiles, changed, removed);
}

void FileScanner::workerMain(AppConfig config, bool runStageB) {
    int total = 0, changed = 0, removed = 0;
    const auto t0 = std::chrono::steady_clock::now();
    m_stageARunning.store(true, std::memory_order_release);
    emit progressChanged({true, false, 0, 0, QStringLiteral("正在快速扫描 NAS 录像目录...")});

    const bool ok = doStageA(config, total, changed, removed);
    m_stageARunning.store(false, std::memory_order_release);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (!ok || m_stopRequested.load()) {
        if (!ok && !m_stopRequested.load()) {
            emit scanError(QStringLiteral("无法建立录像索引。请检查应用数据目录的写入权限和剩余空间，再重新扫描。"));
        }
        emit progressChanged({false, false, total, 0, QStringLiteral("扫描已取消或失败")});
        return;
    }

    MCS_LOG_INFO("SCAN", QStringLiteral("Stage A 完成: 共 %1 个录像, 新增/更新 %2, 清理 %3, 耗时 %4 ms")
                             .arg(total).arg(changed).arg(removed).arg(ms, 0, 'f', 1));
    emit stageACompleted(total, changed, removed);

    if (runStageB && !m_stopRequested.load()) {
        emit progressChanged({true, true, total, 0, QStringLiteral("正在后台校验录像元数据...")});
        doStageB();
    } else {
        emit progressChanged({false, false, total, 0, QStringLiteral("索引就绪 (%1 条录像)").arg(total)});
    }
}

bool FileScanner::doStageA(const AppConfig& config, int& outTotal, int& outChanged, int& outRemoved) {
    if (!m_db || !m_db->isOpen()) return false;

    if (!m_db->syncStreamsConfig(config.cameras, config.streams)) return false;
    const auto existing = m_db->loadExistingFilesMeta();
    const int64_t scanGen = TimeUtils::nowEpochMs();
    const int64_t nowMs = scanGen;

    // 映射 cameraId + ":" + channel -> StreamConfig
    QHash<QString, StreamConfig> streamMap;
    QHash<QString, StreamConfig> singleFallbackMap;
    for (const auto& s : config.streams) {
        streamMap.insert(s.streamKey, s);
        if (!singleFallbackMap.contains(s.cameraId)) {
            singleFallbackMap.insert(s.cameraId, s);
        }
    }

    QVector<SegmentRecord> toUpsert;
    QVector<int64_t> unchangedIds;
    toUpsert.reserve(2048);
    unchangedIds.reserve(existing.size());

    outTotal = 0;
    outChanged = 0;
    QVector<int> reachableStreamIndices;

    for (const auto& cam : config.cameras) {
        if (m_stopRequested.load(std::memory_order_acquire)) return false;
        const auto rootFs = toFsPath(cam.rootPath);
        std::error_code ec;
        if (!std::filesystem::is_directory(rootFs, ec)) {
            const QString msg = QStringLiteral("%1：无法打开录像目录 %2。请确认 NAS 已连接，且该文件夹可以在系统文件管理器中打开。")
                .arg(cam.displayName, cam.rootPath);
            MCS_LOG_WARN("SCAN", msg);
            emit scanError(msg);
            continue; // 保留该摄像头历史有效索引，不误删
        }

        const int totalBeforeCamera = outTotal;
        int corruptInCamera = 0;
        bool metadataReadFailed = false;
        for (std::filesystem::recursive_directory_iterator it(rootFs, ec), end;
             it != end && !ec; it.increment(ec)) {
            if (m_stopRequested.load(std::memory_order_acquire)) return false;

            const auto& entry = *it;
            if (!entry.is_regular_file(ec)) {
                metadataReadFailed |= bool(ec);
                ec.clear();
                continue;
            }

            const QString fileName = fromFsPath(entry.path().filename());
            if (!fileName.endsWith(QLatin1String(".mp4"), Qt::CaseInsensitive)) continue;

            const auto parsed = TimeUtils::parseXiaomiFilename(fileName, config.timezoneOffsetSec);
            if (!parsed.has_value()) {
                continue;
            }

            const QString key = cam.cameraId + QLatin1Char(':') + parsed->channel;
            auto stIt = streamMap.constFind(key);
            if (stIt == streamMap.constEnd() && !cam.isDual) {
                stIt = singleFallbackMap.constFind(cam.cameraId);
            }
            if (stIt == streamMap.constEnd() && stIt == singleFallbackMap.constEnd()) {
                continue;
            }
            const StreamConfig& sc = stIt.value();

            const QString fullPath = fromFsPath(entry.path());
            ec.clear();
            const auto szRaw = entry.file_size(ec);
            if (ec) {
                metadataReadFailed = true;
                ec.clear();
                continue;
            }
            ec.clear();
            const auto wtRaw = entry.last_write_time(ec);
            if (ec) {
                metadataReadFailed = true;
                ec.clear();
                continue;
            }
            const int64_t fsize = static_cast<int64_t>(szRaw);
            const int64_t mtimeMs = fileTimeToEpochMs(wtRaw);

            ++outTotal;
            auto exIt = existing.constFind(fullPath);
            if (exIt != existing.constEnd()) {
                const auto& meta = exIt.value();
                const bool stillWriting = (nowMs - mtimeMs < 15000 && fsize < 134217728LL);
                if (meta.fileSize == fsize && meta.mtimeMs == mtimeMs && !stillWriting &&
                    meta.status != SegmentStatus::Writing) {
                    unchangedIds.append(meta.id);
                    continue;
                }
            }

            SegmentRecord rec;
            rec.streamIndex = sc.streamIndex;
            rec.streamKey = sc.streamKey;
            rec.path = fullPath;
            rec.nominalStartMs = parsed->nominalStartMs;
            rec.nominalEndMs = parsed->nominalEndMs;
            rec.fileSize = fsize;
            rec.mtimeMs = mtimeMs;
            if (fsize < 1024) {
                rec.status = SegmentStatus::Corrupt;
                ++corruptInCamera;
            } else if (nowMs - mtimeMs < 15000 && fsize < 134217728LL) {
                rec.status = SegmentStatus::Writing;
            } else {
                rec.status = SegmentStatus::Ok;
            }
            toUpsert.append(rec);
            ++outChanged;
        }
        if (corruptInCamera > 0) {
            emit scanError(QStringLiteral("%1：有 %2 个录像片段损坏或未写完。请检查文件大小，并尝试用其他播放器打开。")
                .arg(cam.displayName).arg(corruptInCamera));
        }
        if (ec || metadataReadFailed) {
            emit scanError(QStringLiteral("%1：读取录像目录时出错。请检查网络连接及子文件夹的读取权限，然后重新扫描。")
                .arg(cam.displayName));
        } else {
            if (outTotal == totalBeforeCamera) {
                emit scanError(QStringLiteral("%1：未找到可识别的录像。请选择包含 00_年月日时分秒_年月日时分秒.mp4 的文件夹。双摄的第二路使用 10_ 前缀。")
                    .arg(cam.displayName));
            }
            for (const auto& s : config.streams) {
                if (s.cameraId == cam.cameraId) {
                    reachableStreamIndices.append(s.streamIndex);
                }
            }
        }
    }

    if (!unchangedIds.isEmpty()) {
        if (!m_db->touchScanGenBatch(unchangedIds, scanGen)) return false;
    }
    if (!toUpsert.isEmpty()) {
        if (!m_db->insertOrUpdateSegmentsBatch(toUpsert, scanGen)) return false;
    }
    outRemoved = reachableStreamIndices.isEmpty()
        ? 0
        : m_db->deleteMissingSegmentsForStreams(reachableStreamIndices, scanGen);
    return true;
}

bool FileScanner::doCameraQuickScan(const AppConfig& config, int streamIndex, int& outChanged) {
    outChanged = 0;
    if (!m_db || !m_db->isOpen()) return false;

    QString targetCamId;
    for (const auto& s : config.streams) {
        if (s.streamIndex == streamIndex) {
            targetCamId = s.cameraId;
            break;
        }
    }
    if (targetCamId.isEmpty()) return false;

    std::optional<CameraConfig> targetCam;
    for (const auto& cam : config.cameras) {
        if (cam.cameraId == targetCamId) {
            targetCam = cam;
            break;
        }
    }
    if (!targetCam) return false;

    const auto rootFs = toFsPath(targetCam->rootPath);
    std::error_code ec;
    if (!std::filesystem::exists(rootFs, ec)) {
        return false;
    }

    QVector<int> camStreamIndices;
    QHash<QString, StreamConfig> streamMap;
    QHash<QString, StreamConfig> singleFallbackMap;
    for (const auto& s : config.streams) {
        if (s.cameraId == targetCamId) {
            camStreamIndices.append(s.streamIndex);
            streamMap.insert(s.streamKey, s);
            if (!singleFallbackMap.contains(s.cameraId)) {
                singleFallbackMap.insert(s.cameraId, s);
            }
        }
    }

    const auto existing = m_db->loadExistingFilesMetaForStreams(camStreamIndices);
    const int64_t scanGen = TimeUtils::nowEpochMs();
    const int64_t nowMs = scanGen;

    QVector<SegmentRecord> toUpsert;
    QVector<int64_t> unchangedIds;

    auto options = std::filesystem::directory_options::skip_permission_denied;
    for (std::filesystem::recursive_directory_iterator it(rootFs, options, ec), end;
         it != end && !ec; it.increment(ec)) {
        if (m_stopRequested.load(std::memory_order_acquire)) return false;

        const auto& entry = *it;
        if (!entry.is_regular_file(ec)) continue;

        const QString fileName = fromFsPath(entry.path().filename());
        if (!fileName.endsWith(QLatin1String(".mp4"), Qt::CaseInsensitive)) continue;

        const auto parsed = TimeUtils::parseXiaomiFilename(fileName, config.timezoneOffsetSec);
        if (!parsed.has_value()) continue;

        const QString key = targetCam->cameraId + QLatin1Char(':') + parsed->channel;
        auto stIt = streamMap.constFind(key);
        if (stIt == streamMap.constEnd() && !targetCam->isDual) {
            stIt = singleFallbackMap.constFind(targetCam->cameraId);
        }
        if (stIt == streamMap.constEnd() && stIt == singleFallbackMap.constEnd()) continue;
        const StreamConfig& sc = stIt.value();

        const QString fullPath = fromFsPath(entry.path());
        ec.clear();
        const auto szRaw = entry.file_size(ec);
        if (ec) {
            ec.clear();
            continue;
        }
        ec.clear();
        const auto wtRaw = entry.last_write_time(ec);
        if (ec) {
            ec.clear();
            continue;
        }
        const int64_t fsize = static_cast<int64_t>(szRaw);
        const int64_t mtimeMs = fileTimeToEpochMs(wtRaw);

        auto exIt = existing.constFind(fullPath);
        if (exIt != existing.constEnd()) {
            const auto& meta = exIt.value();
            const bool stillWriting = (nowMs - mtimeMs < 15000 && fsize < 134217728LL);
            if (meta.fileSize == fsize && meta.mtimeMs == mtimeMs && !stillWriting &&
                meta.status != SegmentStatus::Writing) {
                unchangedIds.append(meta.id);
                continue;
            }
        }

        SegmentRecord rec;
        rec.streamIndex = sc.streamIndex;
        rec.streamKey = sc.streamKey;
        rec.path = fullPath;
        rec.nominalStartMs = parsed->nominalStartMs;
        rec.nominalEndMs = parsed->nominalEndMs;
        rec.fileSize = fsize;
        rec.mtimeMs = mtimeMs;
        if (fsize < 1024) {
            rec.status = SegmentStatus::Corrupt;
        } else if (nowMs - mtimeMs < 15000 && fsize < 134217728LL) {
            rec.status = SegmentStatus::Writing;
        } else {
            rec.status = SegmentStatus::Ok;
        }
        toUpsert.append(rec);
        ++outChanged;
    }

    if (!unchangedIds.isEmpty()) {
        m_db->touchScanGenBatch(unchangedIds, scanGen);
    }
    if (!toUpsert.isEmpty()) {
        m_db->insertOrUpdateSegmentsBatch(toUpsert, scanGen);
    }
    if (!ec) {
        m_db->deleteMissingSegmentsForStreams(camStreamIndices, scanGen);
    }
    return true;
}

void FileScanner::doStageB() {
    if (!m_db || !m_cache) return;
    int totalProbed = 0;
    int corruptCount = 0;

    while (!m_stopRequested.load(std::memory_order_acquire)) {
        const auto batch = m_db->loadPendingProbeSegments(500);
        if (batch.isEmpty()) break;

        QVector<IndexDatabase::ProbeUpdate> updates;
        updates.reserve(50);

        for (const auto& seg : batch) {
            if (m_stopRequested.load(std::memory_order_acquire)) return;

            // 前台正在播放或 Seek 时主动让路，绝不抢占 NAS 机械盘 I/O
            while ((m_foregroundBusy.load(std::memory_order_relaxed) ||
                    m_cache->activeForegroundCount() > 0) &&
                   !m_stopRequested.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(40));
            }
            if (m_stopRequested.load(std::memory_order_acquire)) return;

            const auto sidx = m_cache->getOrLoadSidx(
                seg.path, static_cast<uint64_t>(seg.fileSize), IoPriority::Background);

            IndexDatabase::ProbeUpdate u;
            u.id = seg.id;
            if (!sidx || !sidx->valid) {
                u.status = SegmentStatus::Corrupt;
                u.realDurationMs = std::max<int64_t>(1, seg.nominalEndMs - seg.nominalStartMs);
                u.isFragmented = false;
                ++corruptCount;
                MCS_LOG_WARN("SCAN", QStringLiteral("检测到损坏或无法解析头部的录像: %1").arg(seg.path));
            } else if (sidx->isFragmented && sidx->totalDurationSec > 0.0) {
                u.status = SegmentStatus::Ok;
                u.realDurationMs = static_cast<int64_t>(sidx->totalDurationSec * 1000.0 + 0.5);
                u.isFragmented = true;
            } else {
                u.status = SegmentStatus::Ok;
                u.realDurationMs = std::max<int64_t>(1000, seg.nominalEndMs - seg.nominalStartMs);
                u.isFragmented = false;
            }

            updates.append(u);
            ++totalProbed;

            if (updates.size() >= 50) {
                m_db->updateSegmentProbeBatch(updates);
                updates.clear();
                emit progressChanged({false, true, 0, totalProbed,
                                      QStringLiteral("后台元数据校验中 (%1)...").arg(totalProbed)});
            }
        }

        if (!updates.isEmpty()) {
            m_db->updateSegmentProbeBatch(updates);
        }
    }

    MCS_LOG_INFO("SCAN", QStringLiteral("Stage B 后台校验完成: 处理 %1 个切片, 损坏 %2 个")
                             .arg(totalProbed).arg(corruptCount));
    emit progressChanged({false, false, 0, totalProbed, QStringLiteral("元数据校验完成")});
    emit stageBCompleted(totalProbed, corruptCount);
}

} // namespace mcs
