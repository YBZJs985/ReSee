#include "health/HealthMonitor.h"
#include "core/Logger.h"
#include "core/PlatformNotifier.h"
#include "core/TimeUtils.h"
#include "index/IndexDatabase.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <algorithm>
#include <chrono>
#include <filesystem>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

#ifdef SYNC_PLATFORM_WINDOWS
#include <windows.h>
#else
#include <sys/statvfs.h>
#endif

namespace mcs {

namespace {

std::filesystem::path toFsPath(const QString& p) {
#ifdef SYNC_PLATFORM_WINDOWS
    return std::filesystem::path(p.toStdWString());
#else
    return std::filesystem::path(p.toUtf8().constData());
#endif
}

QString fromFsPath(const std::filesystem::path& p) {
#ifdef SYNC_PLATFORM_WINDOWS
    return QString::fromStdWString(p.wstring()).replace(QLatin1Char('\\'), QLatin1Char('/'));
#else
    return QString::fromUtf8(p.u8string().c_str());
#endif
}

int64_t fileTimeToEpochMs(std::filesystem::file_time_type ft) {
    using namespace std::chrono;
    const auto sysNow = system_clock::now();
    const auto fileNow = std::filesystem::file_time_type::clock::now();
    const auto sysTime = time_point_cast<milliseconds>(sysNow + (ft - fileNow));
    return sysTime.time_since_epoch().count();
}

HealthStatus classifyAge(int64_t ageSec, int onlineMaxMin, int offlineMaxMin) {
    const double mins = static_cast<double>(std::max<int64_t>(0, ageSec)) / 60.0;
    if (mins <= static_cast<double>(onlineMaxMin)) return HealthStatus::Normal;
    if (mins <= static_cast<double>(offlineMaxMin)) return HealthStatus::Delayed;
    return HealthStatus::Offline;
}

QString ensureThumbForCamera(const QString& dataDir, const QString& cameraId, const QString& latestVideoPath) {
    if (dataDir.isEmpty() || cameraId.isEmpty()) return {};
    const QString thumbDir = QDir(dataDir).filePath(QStringLiteral("thumbs"));
    QDir().mkpath(thumbDir);
    const QString jpgPath = QDir(thumbDir).filePath(cameraId + QStringLiteral(".jpg"));
    const QString srcMetaPath = QDir(thumbDir).filePath(cameraId + QStringLiteral(".src"));

    if (latestVideoPath.isEmpty()) {
        return QFile::exists(jpgPath) ? jpgPath : QString();
    }

    // 源文件未变更且缩略图已存在，直接复用缓存
    if (QFile::exists(jpgPath) && QFile::exists(srcMetaPath)) {
        QFile sf(srcMetaPath);
        if (sf.open(QIODevice::ReadOnly)) {
            if (QString::fromUtf8(sf.readAll()).trimmed() == latestVideoPath) {
                return jpgPath;
            }
        }
    }

    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, latestVideoPath.toUtf8().constData(), nullptr, nullptr) < 0) {
        return QFile::exists(jpgPath) ? jpgPath : QString();
    }
    bool saved = false;
    if (avformat_find_stream_info(fmt, nullptr) >= 0) {
        int vIdx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (vIdx >= 0) {
            const AVCodecParameters* par = fmt->streams[vIdx]->codecpar;
            if (const AVCodec* dec = avcodec_find_decoder(par->codec_id)) {
                AVCodecContext* ctx = avcodec_alloc_context3(dec);
                if (ctx && avcodec_parameters_to_context(ctx, par) >= 0 && avcodec_open2(ctx, dec, nullptr) >= 0) {
                    AVPacket* pkt = av_packet_alloc();
                    AVFrame* frm = av_frame_alloc();
                    int tries = 0;
                    while (tries < 40 && av_read_frame(fmt, pkt) >= 0) {
                        if (pkt->stream_index == vIdx) {
                            ++tries;
                            if (avcodec_send_packet(ctx, pkt) >= 0 && avcodec_receive_frame(ctx, frm) >= 0) {
                                const int dw = 176;
                                const int dh = std::max(60, static_cast<int>(std::llround(static_cast<double>(frm->height) * dw / std::max(1, frm->width))));
                                SwsContext* sws = sws_getContext(frm->width, frm->height, static_cast<AVPixelFormat>(frm->format),
                                                                 dw, dh, AV_PIX_FMT_RGB24, SWS_BILINEAR, nullptr, nullptr, nullptr);
                                if (sws) {
                                    QImage img(dw, dh, QImage::Format_RGB888);
                                    uint8_t* dst[4] = {img.bits(), nullptr, nullptr, nullptr};
                                    int linesize[4] = {static_cast<int>(img.bytesPerLine()), 0, 0, 0};
                                    sws_scale(sws, frm->data, frm->linesize, 0, frm->height, dst, linesize);
                                    sws_freeContext(sws);
                                    saved = img.save(jpgPath, "JPG", 84);
                                }
                                av_frame_unref(frm);
                                av_packet_unref(pkt);
                                break;
                            }
                        }
                        av_packet_unref(pkt);
                    }
                    av_frame_free(&frm);
                    av_packet_free(&pkt);
                }
                if (ctx) avcodec_free_context(&ctx);
            }
        }
    }
    avformat_close_input(&fmt);

    if (saved) {
        QFile sf(srcMetaPath);
        if (sf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            sf.write(latestVideoPath.toUtf8());
        }
        return jpgPath;
    }
    return QFile::exists(jpgPath) ? jpgPath : QString();
}

} // namespace

HealthMonitor::HealthMonitor(IndexDatabase* db, QObject* parent)
    : QObject(parent), m_db(db) {
    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, &HealthMonitor::triggerCheckNow);
}

HealthMonitor::~HealthMonitor() {
    stop();
}

void HealthMonitor::configure(const AppConfig& cfg) {
    m_cfg = cfg;
    const int intervalMs = std::clamp(cfg.healthCheckIntervalMin, 1, 120) * 60 * 1000;
    m_timer->setInterval(intervalMs);
    if (cfg.healthAutoCheck) {
        if (!m_timer->isActive()) m_timer->start();
    } else {
        m_timer->stop();
    }
}

void HealthMonitor::start() {
    m_stopRequested.store(false, std::memory_order_release);
    configure(m_cfg);
    // 启动 1.2 秒后做一次低优先级初始健康检查，避开启动瞬间的界面和首帧加载峰值
    QTimer::singleShot(1200, this, &HealthMonitor::triggerCheckNow);
}

void HealthMonitor::stop() {
    if (m_timer) m_timer->stop();
    m_stopRequested.store(true, std::memory_order_release);
    if (m_worker.joinable()) {
        m_worker.join();
    }
}

void HealthMonitor::setForegroundBusy(bool busy) {
    m_foregroundBusy.store(busy, std::memory_order_relaxed);
}

void HealthMonitor::triggerCheckNow() {
    bool expected = false;
    if (!m_checking.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return; // 已有检测在进行中，防重复
    }
    emit checkStarted();

    if (m_worker.joinable()) {
        m_worker.join();
    }
    const AppConfig cfgCopy = m_cfg;
    m_worker = std::jthread([this, cfgCopy]() {
        const HealthSnapshot snap = runSingleCheck(cfgCopy);
        m_checking.store(false, std::memory_order_release);

        if (m_stopRequested.load(std::memory_order_acquire)) return;

        // 仅在非首次检查且 NAS 可访问时处理状态跃迁系统通知
        if (m_hasFirstCheck && snap.nasAccessible) {
            for (const auto& cam : snap.cameras) {
                const auto prevIt = m_prevStatuses.constFind(cam.cameraId);
                const HealthStatus prev = (prevIt != m_prevStatuses.constEnd()) ? prevIt.value() : HealthStatus::Unknown;
                if (cfgCopy.healthNotifyOffline &&
                    (prev == HealthStatus::Normal || prev == HealthStatus::Delayed) &&
                    cam.status == HealthStatus::Offline) {
                    PlatformNotifier::showSystemNotification(
                        QStringLiteral("ReSee 监控离线告警"),
                        QStringLiteral("「%1」已超过 %2 分钟未写入新录像，请检查设备或网络。")
                            .arg(cam.displayName)
                            .arg(cam.offlineMaxMin),
                        true);
                } else if (cfgCopy.healthNotifyRecovered &&
                           prev == HealthStatus::Offline &&
                           cam.status == HealthStatus::Normal) {
                    PlatformNotifier::showSystemNotification(
                        QStringLiteral("ReSee 监控恢复通知"),
                        QStringLiteral("「%1」已恢复持续录像写入。").arg(cam.displayName),
                        false);
                }
            }
        }
        m_hasFirstCheck = true;
        for (const auto& cam : snap.cameras) {
            m_prevStatuses.insert(cam.cameraId, cam.status);
        }

        emit snapshotUpdated(snap);
    });
}

HealthSnapshot HealthMonitor::runSingleCheck(const AppConfig& cfg) {
    HealthSnapshot snap;
    snap.checkedAtMs = TimeUtils::nowEpochMs();

    QHash<int, int64_t> streamBytes;
    QHash<int, int64_t> streamCounts;
    if (m_db && m_db->isOpen()) {
        const auto stats = m_db->loadStreamStorageStats();
        for (auto it = stats.cbegin(); it != stats.cend(); ++it) {
            streamBytes.insert(it.key(), it.value().totalBytes);
            streamCounts.insert(it.key(), it.value().fileCount);
        }
    }

    snap.disk = readNasDisk(cfg);

    bool anyCamOk = false;
    for (const auto& cam : cfg.cameras) {
        if (m_stopRequested.load(std::memory_order_acquire)) break;

        QVector<StreamConfig> camStreams;
        for (const auto& s : cfg.streams) {
            if (s.cameraId == cam.cameraId) {
                camStreams.append(s);
            }
        }

        CameraHealthInfo ch = scanCameraHealth(
            cam, camStreams, streamBytes, streamCounts, cfg.timezoneOffsetSec, snap.checkedAtMs);
        if (ch.folderAccessible) anyCamOk = true;

        switch (ch.status) {
        case HealthStatus::Normal: ++snap.normalCount; break;
        case HealthStatus::Delayed: ++snap.delayedCount; break;
        case HealthStatus::Offline: ++snap.offlineCount; break;
        case HealthStatus::Unknown: ++snap.unknownCount; break;
        }
        snap.cameras.append(std::move(ch));
    }

    snap.nasAccessible = snap.disk.ok || anyCamOk;
    if (!snap.nasAccessible) {
        // NAS 不可访问时全部强制保持 Unknown，绝不误报离线
        snap.normalCount = 0;
        snap.delayedCount = 0;
        snap.offlineCount = 0;
        snap.unknownCount = snap.cameras.size();
        for (auto& cam : snap.cameras) {
            cam.status = HealthStatus::Unknown;
            for (auto& ch : cam.channels) {
                ch.status = HealthStatus::Unknown;
            }
        }
    }

    return snap;
}

CameraHealthInfo HealthMonitor::scanCameraHealth(const CameraConfig& cam,
                                                 const QVector<StreamConfig>& camStreams,
                                                 const QHash<int, int64_t>& streamBytes,
                                                 const QHash<int, int64_t>& streamCounts,
                                                 int tzOffsetSec,
                                                 int64_t nowMs) {
    CameraHealthInfo res;
    res.cameraId = cam.cameraId;
    res.displayName = cam.displayName;
    res.rootPath = cam.rootPath;
    res.isDual = cam.isDual;
    res.onlineMaxMin = cam.onlineMaxMin;
    res.offlineMaxMin = cam.offlineMaxMin;

    for (const auto& sc : camStreams) {
        res.totalBytes += streamBytes.value(sc.streamIndex, 0);
        res.fileCount += streamCounts.value(sc.streamIndex, 0);
        ChannelHealthInfo chInfo;
        chInfo.streamIndex = sc.streamIndex;
        chInfo.channel = sc.channel;
        chInfo.displayName = sc.displayName;
        res.channels.append(chInfo);
    }

    const auto rootFs = toFsPath(cam.rootPath);
    std::error_code ec;
    if (!std::filesystem::exists(rootFs, ec) || !std::filesystem::is_directory(rootFs, ec)) {
        res.folderAccessible = false;
        res.status = HealthStatus::Unknown;
        res.errorMessage = QStringLiteral("目录不可访问 / NAS 未连接");
        return res;
    }

    res.folderAccessible = true;

    struct BestFile {
        int64_t mtimeMs = 0;
        int64_t startMs = 0;
        int64_t endMs = 0;
        QString fileName;
        QString fullPath;
    };
    QHash<QString, BestFile> bestByChannel;

    // 使用显式深度受限迭代，并礼貌让步于前台 Seek/播放
    QVector<QPair<std::filesystem::path, int>> stack;
    stack.append({rootFs, 0});
    int steps = 0;

    while (!stack.isEmpty() && !m_stopRequested.load(std::memory_order_acquire)) {
        const auto [curDir, depth] = stack.takeLast();
        std::filesystem::directory_iterator it(
            curDir, std::filesystem::directory_options::skip_permission_denied, ec);
        if (ec) {
            ec.clear();
            continue;
        }
        for (const auto& entry : it) {
            if (m_stopRequested.load(std::memory_order_acquire)) break;
            if ((++steps & 0xFF) == 0 && m_foregroundBusy.load(std::memory_order_relaxed)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(8));
            }
            ec.clear();
            if (entry.is_directory(ec)) {
                if (cam.recursive && depth < cam.maxDepth) {
                    stack.append({entry.path(), depth + 1});
                }
                continue;
            }
            ec.clear();
            if (!entry.is_regular_file(ec)) continue;

            const QString fname = fromFsPath(entry.path().filename());
            if (fname.startsWith(QLatin1Char('.'))) continue;
            if (!fname.endsWith(QLatin1String(".mp4"), Qt::CaseInsensitive)) continue;

            const auto parsed = TimeUtils::parseXiaomiFilename(fname, tzOffsetSec);
            if (!parsed.has_value()) continue;

            ec.clear();
            const auto wt = entry.last_write_time(ec);
            if (ec) continue;
            ec.clear();
            const auto sz = entry.file_size(ec);
            if (ec || sz < 1024) continue;

            const int64_t mtimeMs = fileTimeToEpochMs(wt);
            const QString chKey = cam.isDual ? parsed->channel : QStringLiteral("00");
            auto& bf = bestByChannel[chKey];
            if (mtimeMs > bf.mtimeMs) {
                bf.mtimeMs = mtimeMs;
                bf.startMs = parsed->nominalStartMs;
                bf.endMs = parsed->nominalEndMs;
                bf.fileName = fname;
                bf.fullPath = fromFsPath(entry.path());
            }
        }
    }

    if (bestByChannel.isEmpty()) {
        res.status = HealthStatus::Unknown;
        res.errorMessage = QStringLiteral("目录内暂无有效录像");
        return res;
    }

    // 汇总各通道健康状态（双摄 00 / 10 独立评估）
    HealthStatus worstStatus = HealthStatus::Normal;
    HealthStatus bestStatus = HealthStatus::Unknown;
    int64_t newestMtime = 0;
    QString newestFullPath;

    for (auto& ch : res.channels) {
        const QString lookupKey = cam.isDual ? ch.channel : QStringLiteral("00");
        if (!bestByChannel.contains(lookupKey)) {
            ch.status = HealthStatus::Unknown;
            worstStatus = std::max(worstStatus, HealthStatus::Offline);
            continue;
        }
        const auto& bf = bestByChannel[lookupKey];
        ch.lastWriteMs = bf.mtimeMs;
        ch.ageSec = std::max<int64_t>(0, (nowMs - bf.mtimeMs) / 1000);
        ch.status = classifyAge(ch.ageSec, cam.onlineMaxMin, cam.offlineMaxMin);
        ch.latestFileName = bf.fileName;
        ch.latestFilePath = bf.fullPath;
        ch.latestRangeText = QStringLiteral("%1 ~ %2")
                                 .arg(TimeUtils::formatEpochMs(bf.startMs, tzOffsetSec),
                                      TimeUtils::formatTimeOnlyMs(bf.endMs, tzOffsetSec));
        ch.lastJumpMs = std::max(bf.startMs + 300, bf.endMs - 3000);

        if (bf.mtimeMs > newestMtime) {
            newestMtime = bf.mtimeMs;
            newestFullPath = bf.fullPath;
            res.lastWriteMs = bf.mtimeMs;
            res.ageSec = ch.ageSec;
            res.latestFileName = bf.fileName;
            res.latestRangeText = ch.latestRangeText;
            res.lastJumpMs = ch.lastJumpMs;
        }
        if (static_cast<int>(ch.status) > static_cast<int>(worstStatus) && ch.status != HealthStatus::Unknown) {
            worstStatus = ch.status;
        }
        if (bestStatus == HealthStatus::Unknown || static_cast<int>(ch.status) < static_cast<int>(bestStatus)) {
            bestStatus = ch.status;
        }

        // 若健康检查发现新写入文件的名义时间晚于当前索引或 2 分钟内刚落盘，通知局部刷新
        if (ch.ageSec <= 180) {
            emit newFilesDetectedOnCamera(ch.streamIndex);
        }
    }

    if (cam.isDual && res.channels.size() >= 2 &&
        res.channels[0].status != res.channels[1].status) {
        res.partialAbnormal = true;
    }
    res.status = (newestMtime > 0) ? worstStatus : HealthStatus::Unknown;
    if (!newestFullPath.isEmpty()) {
        res.thumbPath = ensureThumbForCamera(m_cfg.dataDir, cam.cameraId, newestFullPath);
    }
    return res;
}

NasDiskInfo HealthMonitor::readNasDisk(const AppConfig& cfg) const {
    NasDiskInfo info;
    QString root;
    for (const auto& c : cfg.cameras) {
        if (!c.rootPath.isEmpty()) {
#ifdef SYNC_PLATFORM_WINDOWS
            if (c.rootPath.size() >= 2 && c.rootPath[1] == QLatin1Char(':')) {
                root = c.rootPath.left(2) + QStringLiteral("/");
                break;
            }
#else
            root = c.rootPath;
            break;
#endif
        }
    }
    if (root.isEmpty()) {
        info.errorMessage = QStringLiteral("未配置 NAS 路径");
        return info;
    }
    info.rootPath = root;

#ifdef SYNC_PLATFORM_WINDOWS
    ULARGE_INTEGER avail{}, total{}, freeB{};
    std::wstring wRoot = QDir::toNativeSeparators(root).toStdWString();
    if (!GetDiskFreeSpaceExW(wRoot.c_str(), &avail, &total, &freeB) || total.QuadPart == 0) {
        info.ok = false;
        info.errorMessage = QStringLiteral("无法读取 NAS 空间");
        info.levelCode = 2;
        info.levelText = QStringLiteral("未知");
        return info;
    }
    info.ok = true;
    info.totalBytes = static_cast<int64_t>(total.QuadPart);
    info.freeBytes = static_cast<int64_t>(freeB.QuadPart);
    info.usedBytes = std::max<int64_t>(0, info.totalBytes - info.freeBytes);
#else
    struct statvfs st{};
    if (statvfs(root.toUtf8().constData(), &st) != 0 || st.f_blocks == 0) {
        info.ok = false;
        info.errorMessage = QStringLiteral("无法读取 NAS 空间");
        info.levelCode = 2;
        info.levelText = QStringLiteral("未知");
        return info;
    }
    info.ok = true;
    info.totalBytes = static_cast<int64_t>(st.f_blocks) * static_cast<int64_t>(st.f_frsize);
    info.freeBytes = static_cast<int64_t>(st.f_bavail) * static_cast<int64_t>(st.f_frsize);
    info.usedBytes = std::max<int64_t>(0, info.totalBytes - info.freeBytes);
#endif

    info.freeRatio = info.totalBytes > 0 ? static_cast<double>(info.freeBytes) / static_cast<double>(info.totalBytes) : 0.0;
    if (info.freeRatio >= 0.40) {
        info.levelCode = 0;
        info.levelText = QStringLiteral("空间从容");
    } else if (info.freeRatio >= 0.20) {
        info.levelCode = 1;
        info.levelText = QStringLiteral("空间偏紧");
    } else {
        info.levelCode = 2;
        info.levelText = QStringLiteral("空间紧张");
    }
    return info;
}

} // namespace mcs
