#include "core/AppConfig.h"

#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

namespace mcs {

namespace {
QString defaultBaseDataDir() {
    const QString overrideDir = qEnvironmentVariable("RESEE_DATA_DIR");
    if (!overrideDir.isEmpty() && QDir::isAbsolutePath(overrideDir)) {
        return QDir::cleanPath(overrideDir);
    }
    const QString loc = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return loc.isEmpty() ? (QDir::homePath() + QStringLiteral("/.resee")) : loc;
}
} // namespace

AppConfig AppConfig::createDefault() {
    AppConfig cfg;
    cfg.dataDir = defaultBaseDataDir();
    cfg.dbPath = cfg.dataDir + QStringLiteral("/index.db");
    cfg.cacheDir = cfg.dataDir + QStringLiteral("/cache");
    cfg.logDir = cfg.dataDir + QStringLiteral("/logs");
    cfg.screenshotDir = cfg.dataDir + QStringLiteral("/screenshots");

    // 开源通用版默认不预置私人摄像头，由用户在界面中自行添加
    cfg.cameras.clear();
    cfg.rebuildStreamsFromCameras();
    return cfg;
}

void AppConfig::rebuildStreamsFromCameras() {
    QMap<QString, int64_t> oldOffsets;
    QMap<QString, QString> oldNames;
    for (const auto& s : streams) {
        oldOffsets.insert(s.streamKey, s.defaultOffsetMs);
        oldNames.insert(s.streamKey, s.displayName);
    }

    streams.clear();
    int idx = 0;
    for (const auto& cam : cameras) {
        if (cam.isDual) {
            for (const QString& ch : {QStringLiteral("00"), QStringLiteral("10")}) {
                StreamConfig sc;
                sc.streamIndex = idx++;
                sc.streamKey = cam.cameraId + QLatin1Char(':') + ch;
                sc.cameraId = cam.cameraId;
                sc.channel = ch;
                const QString defName = (ch == QLatin1String("00"))
                    ? (cam.displayName + QStringLiteral(" · 通道00"))
                    : (cam.displayName + QStringLiteral(" · 通道10"));
                sc.displayName = oldNames.value(sc.streamKey, defName);
                sc.rootPath = cam.rootPath;
                sc.defaultOffsetMs = oldOffsets.value(sc.streamKey, 0);
                streams.append(sc);
            }
        } else {
            StreamConfig sc;
            sc.streamIndex = idx++;
            sc.streamKey = cam.cameraId + QStringLiteral(":00");
            sc.cameraId = cam.cameraId;
            sc.channel = QStringLiteral("00");
            sc.displayName = oldNames.value(sc.streamKey, cam.displayName);
            sc.rootPath = cam.rootPath;
            sc.defaultOffsetMs = oldOffsets.value(sc.streamKey, 0);
            streams.append(sc);
        }
    }
}

QString AppConfig::configFilePath() const {
    return QDir(dataDir).filePath(QStringLiteral("config.json"));
}

bool AppConfig::loadFromJson(const QString& jsonPath) {
    QFile f(jsonPath);
    if (!f.exists() || !f.open(QIODevice::ReadOnly)) {
        return false;
    }
    const auto doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject()) return false;

    const auto obj = doc.object();
    developerDiagnostics = obj.value(QStringLiteral("developerDiagnostics")).toBool(false);
    if (obj.contains(QStringLiteral("cacheDir"))) {
        QString cd = obj.value(QStringLiteral("cacheDir")).toString();
        if (cd.endsWith(QLatin1String("/MultiCamSyncPlayer/cache")) && !QDir(cd).exists()) {
            cd = cacheDir;
        }
        if (!cd.isEmpty()) cacheDir = cd;
    }
    if (obj.contains(QStringLiteral("cacheMaxGb"))) {
        cacheMaxGb = qBound(1, obj.value(QStringLiteral("cacheMaxGb")).toInt(cacheMaxGb), 500);
    }
    if (obj.contains(QStringLiteral("memCacheMaxMb"))) {
        memCacheMaxMb = qBound(128, obj.value(QStringLiteral("memCacheMaxMb")).toInt(memCacheMaxMb), 8192);
    }
    if (obj.contains(QStringLiteral("maxConcurrentNasReads"))) {
        maxConcurrentNasReads = qBound(1, obj.value(QStringLiteral("maxConcurrentNasReads")).toInt(maxConcurrentNasReads), 8);
    }
    if (obj.contains(QStringLiteral("timezoneOffsetSec"))) {
        timezoneOffsetSec = obj.value(QStringLiteral("timezoneOffsetSec")).toInt(timezoneOffsetSec);
    }
    if (obj.contains(QStringLiteral("preferHwDecode"))) {
        preferHwDecode = obj.value(QStringLiteral("preferHwDecode")).toBool(preferHwDecode);
    }
    if (obj.contains(QStringLiteral("defaultStepSec"))) {
        defaultStepSec = qBound(1, obj.value(QStringLiteral("defaultStepSec")).toInt(defaultStepSec), 60);
    }

    if (obj.contains(QStringLiteral("cameras")) && obj.value(QStringLiteral("cameras")).isArray()) {
        QVector<CameraConfig> loadedCams;
        const auto arr = obj.value(QStringLiteral("cameras")).toArray();
        for (int i = 0; i < arr.size(); ++i) {
            const auto cObj = arr.at(i).toObject();
            CameraConfig cc;
            cc.cameraId = cObj.value(QStringLiteral("cameraId")).toString();
            cc.displayName = cObj.value(QStringLiteral("displayName")).toString();
            cc.rootPath = cObj.value(QStringLiteral("rootPath")).toString();
            cc.isDual = cObj.value(QStringLiteral("isDual")).toBool(false);
            cc.sortOrder = cObj.value(QStringLiteral("sortOrder")).toInt(i);
            cc.onlineMaxMin = qBound(5, cObj.value(QStringLiteral("onlineMaxMin")).toInt(30), 1440);
            cc.offlineMaxMin = qBound(cc.onlineMaxMin + 5, cObj.value(QStringLiteral("offlineMaxMin")).toInt(60), 2880);
            cc.recursive = cObj.value(QStringLiteral("recursive")).toBool(cc.isDual);
            cc.maxDepth = qBound(1, cObj.value(QStringLiteral("maxDepth")).toInt(cc.isDual ? 3 : 1), 8);
            if (!cc.cameraId.isEmpty() && !cc.rootPath.isEmpty()) {
                loadedCams.append(cc);
            }
        }
        cameras = loadedCams;
        rebuildStreamsFromCameras();
    }

    if (obj.contains(QStringLiteral("streamDisplayNames")) && obj.value(QStringLiteral("streamDisplayNames")).isObject()) {
        const auto nmObj = obj.value(QStringLiteral("streamDisplayNames")).toObject();
        for (auto& s : streams) {
            if (nmObj.contains(s.streamKey)) {
                const QString n = nmObj.value(s.streamKey).toString().trimmed();
                if (!n.isEmpty()) s.displayName = n;
            }
        }
    }

    audioFollowsFocus = obj.value(QStringLiteral("audioFollowsFocus")).toBool(audioFollowsFocus);
    manualGridAudioStream = obj.value(QStringLiteral("manualGridAudioStream")).toInt(manualGridAudioStream);
    lastDualAudioStream = obj.value(QStringLiteral("lastDualAudioStream")).toInt(lastDualAudioStream);
    audioVolume = qBound(0.0, obj.value(QStringLiteral("audioVolume")).toDouble(audioVolume), 1.0);
    audioMuted = obj.value(QStringLiteral("audioMuted")).toBool(audioMuted);

    healthAutoCheck = obj.value(QStringLiteral("healthAutoCheck")).toBool(healthAutoCheck);
    healthCheckIntervalMin = qBound(1, obj.value(QStringLiteral("healthCheckIntervalMin")).toInt(healthCheckIntervalMin), 120);
    healthNotifyOffline = obj.value(QStringLiteral("healthNotifyOffline")).toBool(healthNotifyOffline);
    healthNotifyRecovered = obj.value(QStringLiteral("healthNotifyRecovered")).toBool(healthNotifyRecovered);

    locationConfigured = obj.value(QStringLiteral("locationConfigured")).toBool(locationConfigured);
    locationCity = obj.value(QStringLiteral("locationCity")).toString(locationCity);
    latitude = qBound(-89.9, obj.value(QStringLiteral("latitude")).toDouble(latitude), 89.9);
    longitude = qBound(-180.0, obj.value(QStringLiteral("longitude")).toDouble(longitude), 180.0);

    const QString dayCol = obj.value(QStringLiteral("timelineDayColor")).toString(timelineDayColor);
    if (dayCol.startsWith(QLatin1Char('#')) && (dayCol.size() == 7 || dayCol.size() == 9)) {
        timelineDayColor = dayCol;
    }
    const QString nightCol = obj.value(QStringLiteral("timelineNightColor")).toString(timelineNightColor);
    if (nightCol.startsWith(QLatin1Char('#')) && (nightCol.size() == 7 || nightCol.size() == 9)) {
        timelineNightColor = nightCol;
    }

    if (obj.contains(QStringLiteral("streamOffsets")) && obj.value(QStringLiteral("streamOffsets")).isObject()) {
        const auto offObj = obj.value(QStringLiteral("streamOffsets")).toObject();
        for (auto& s : streams) {
            if (offObj.contains(s.streamKey)) {
                s.defaultOffsetMs = offObj.value(s.streamKey).toInteger(0);
            }
        }
    }

    lastMasterTimeMs = obj.value(QStringLiteral("lastMasterTimeMs")).toInteger(0);
    lastPlaybackRate = obj.value(QStringLiteral("lastPlaybackRate")).toDouble(1.0);
    lastLayoutMode = obj.value(QStringLiteral("lastLayoutMode")).toString(QStringLiteral("grouped"));
    lastFocusedStream = obj.value(QStringLiteral("lastFocusedStream")).toInt(-1);
    timelineExpanded = obj.value(QStringLiteral("timelineExpanded")).toBool(timelineExpanded);
    diagnosticsVisible = obj.value(QStringLiteral("diagnosticsVisible")).toBool(false);

    if (obj.contains(QStringLiteral("shortcuts")) && obj.value(QStringLiteral("shortcuts")).isObject()) {
        const auto scObj = obj.value(QStringLiteral("shortcuts")).toObject();
        shortcuts.togglePlay = scObj.value(QStringLiteral("togglePlay")).toString(shortcuts.togglePlay);
        shortcuts.addBookmark = scObj.value(QStringLiteral("addBookmark")).toString(shortcuts.addBookmark);
        shortcuts.stepBackward = scObj.value(QStringLiteral("stepBackward")).toString(shortcuts.stepBackward);
        shortcuts.stepForward = scObj.value(QStringLiteral("stepForward")).toString(shortcuts.stepForward);
        shortcuts.exitFocus = scObj.value(QStringLiteral("exitFocus")).toString(shortcuts.exitFocus);
    }

    return true;
}

bool AppConfig::saveToJson(const QString& jsonPath) const {
    QDir().mkpath(QFileInfo(jsonPath).absolutePath());
    QJsonObject obj;
    obj.insert(QStringLiteral("cacheDir"), cacheDir);
    obj.insert(QStringLiteral("developerDiagnostics"), developerDiagnostics);
    obj.insert(QStringLiteral("cacheMaxGb"), cacheMaxGb);
    obj.insert(QStringLiteral("memCacheMaxMb"), memCacheMaxMb);
    obj.insert(QStringLiteral("maxConcurrentNasReads"), maxConcurrentNasReads);
    obj.insert(QStringLiteral("timezoneOffsetSec"), timezoneOffsetSec);
    obj.insert(QStringLiteral("preferHwDecode"), preferHwDecode);
    obj.insert(QStringLiteral("defaultStepSec"), defaultStepSec);

    QJsonArray camArr;
    for (const auto& c : cameras) {
        QJsonObject co;
        co.insert(QStringLiteral("cameraId"), c.cameraId);
        co.insert(QStringLiteral("displayName"), c.displayName);
        co.insert(QStringLiteral("rootPath"), c.rootPath);
        co.insert(QStringLiteral("isDual"), c.isDual);
        co.insert(QStringLiteral("sortOrder"), c.sortOrder);
        co.insert(QStringLiteral("onlineMaxMin"), c.onlineMaxMin);
        co.insert(QStringLiteral("offlineMaxMin"), c.offlineMaxMin);
        co.insert(QStringLiteral("recursive"), c.recursive);
        co.insert(QStringLiteral("maxDepth"), c.maxDepth);
        camArr.append(co);
    }
    obj.insert(QStringLiteral("cameras"), camArr);

    QJsonObject nmObj;
    for (const auto& s : streams) {
        nmObj.insert(s.streamKey, s.displayName);
    }
    obj.insert(QStringLiteral("streamDisplayNames"), nmObj);

    obj.insert(QStringLiteral("audioFollowsFocus"), audioFollowsFocus);
    obj.insert(QStringLiteral("manualGridAudioStream"), manualGridAudioStream);
    obj.insert(QStringLiteral("lastDualAudioStream"), lastDualAudioStream);
    obj.insert(QStringLiteral("audioVolume"), audioVolume);
    obj.insert(QStringLiteral("audioMuted"), audioMuted);

    obj.insert(QStringLiteral("healthAutoCheck"), healthAutoCheck);
    obj.insert(QStringLiteral("healthCheckIntervalMin"), healthCheckIntervalMin);
    obj.insert(QStringLiteral("healthNotifyOffline"), healthNotifyOffline);
    obj.insert(QStringLiteral("healthNotifyRecovered"), healthNotifyRecovered);

    obj.insert(QStringLiteral("locationConfigured"), locationConfigured);
    obj.insert(QStringLiteral("locationCity"), locationCity);
    obj.insert(QStringLiteral("latitude"), latitude);
    obj.insert(QStringLiteral("longitude"), longitude);
    obj.insert(QStringLiteral("timelineDayColor"), timelineDayColor);
    obj.insert(QStringLiteral("timelineNightColor"), timelineNightColor);

    QJsonObject offObj;
    for (const auto& s : streams) {
        offObj.insert(s.streamKey, static_cast<qint64>(s.defaultOffsetMs));
    }
    obj.insert(QStringLiteral("streamOffsets"), offObj);

    obj.insert(QStringLiteral("lastMasterTimeMs"), static_cast<qint64>(lastMasterTimeMs));
    obj.insert(QStringLiteral("lastPlaybackRate"), lastPlaybackRate);
    obj.insert(QStringLiteral("lastLayoutMode"), lastLayoutMode);
    obj.insert(QStringLiteral("lastFocusedStream"), lastFocusedStream);
    obj.insert(QStringLiteral("timelineExpanded"), timelineExpanded);
    obj.insert(QStringLiteral("diagnosticsVisible"), diagnosticsVisible);

    QJsonObject scObj;
    scObj.insert(QStringLiteral("togglePlay"), shortcuts.togglePlay);
    scObj.insert(QStringLiteral("addBookmark"), shortcuts.addBookmark);
    scObj.insert(QStringLiteral("stepBackward"), shortcuts.stepBackward);
    scObj.insert(QStringLiteral("stepForward"), shortcuts.stepForward);
    scObj.insert(QStringLiteral("exitFocus"), shortcuts.exitFocus);
    obj.insert(QStringLiteral("shortcuts"), scObj);

    QSaveFile f(jsonPath);
    if (!f.open(QIODevice::WriteOnly)) return false;
    const auto data = QJsonDocument(obj).toJson(QJsonDocument::Indented);
    return f.write(data) == data.size() && f.commit();
}

} // namespace mcs
