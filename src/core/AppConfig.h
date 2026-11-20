#pragma once

#include "core/Types.h"

#include <QMap>
#include <QString>
#include <QVector>

namespace mcs {

struct ShortcutConfig {
    QString togglePlay = QStringLiteral("Space");
    QString addBookmark = QStringLiteral("M");
    QString stepBackward = QStringLiteral("Left");
    QString stepForward = QStringLiteral("Right");
    QString exitFocus = QStringLiteral("Escape");
};

struct AppConfig {
    QString dataDir;
    QString dbPath;
    QString cacheDir;
    bool developerDiagnostics = false;
    QString logDir;
    QString screenshotDir;

    int cacheMaxGb = 15;
    int memCacheMaxMb = 512;
    int maxConcurrentNasReads = 6;
    int timezoneOffsetSec = 8 * 3600;
    bool preferHwDecode = true;
    int defaultStepSec = 5;

    // 音频监听配置
    bool audioFollowsFocus = true;
    int manualGridAudioStream = 0;
    int lastDualAudioStream = 0;
    double audioVolume = 0.85;
    bool audioMuted = false;

    // 录像健康监测配置
    bool healthAutoCheck = true;
    int healthCheckIntervalMin = 10;
    bool healthNotifyOffline = true;
    bool healthNotifyRecovered = false;

    // 摄像头所在地（用于离线天文日出日落计算）
    bool locationConfigured = false;
    QString locationCity = QStringLiteral("未配置（固定 06:00–18:00）");
    double latitude = 39.9042;
    double longitude = 116.4074;

    // 时间轴昼夜自定义配色
    QString timelineDayColor = QStringLiteral("#5A5A5A");
    QString timelineNightColor = QStringLiteral("#806EA5");

    QVector<CameraConfig> cameras;
    QVector<StreamConfig> streams;

    // 断点续看与上次 UI 状态
    int64_t lastMasterTimeMs = 0;
    double lastPlaybackRate = 1.0;
    QString lastLayoutMode = QStringLiteral("grouped"); // "grouped" (5卡) 或 "grid6" (6路网格)
    int lastFocusedStream = -1;
    bool timelineExpanded = false;
    bool diagnosticsVisible = false;

    ShortcutConfig shortcuts;

    static AppConfig createDefault();
    bool loadFromJson(const QString& jsonPath);
    bool saveToJson(const QString& jsonPath) const;
    QString configFilePath() const;
    void rebuildStreamsFromCameras();
};

} // namespace mcs
