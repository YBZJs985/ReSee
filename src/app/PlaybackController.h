#pragma once

#include "core/AppConfig.h"
#include "core/Types.h"

#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QUrl>
#include <QTimer>
#include <QVariantList>
#include <array>
#include <memory>

namespace mcs {

class IndexDatabase;
class NasBlockCache;
class FileScanner;
class SyncScheduler;
class TimelineModel;
class DateNavModel;
class BookmarkModel;
class DiagnosticsModel;
class HealthMonitor;
class HealthModel;
class VideoRenderItem;

class PlaybackController : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool playing READ playing NOTIFY clockStateChanged)
    Q_PROPERTY(bool scrubbing READ scrubbing NOTIFY clockStateChanged)
    Q_PROPERTY(double playbackRate READ playbackRate NOTIFY clockStateChanged)
    Q_PROPERTY(qint64 masterTimeMs READ masterTimeMs NOTIFY masterTimeChanged)
    Q_PROPERTY(QString masterDateText READ masterDateText NOTIFY masterTimeChanged)
    Q_PROPERTY(QString masterTimeText READ masterTimeText NOTIFY masterTimeChanged)
    Q_PROPERTY(QString masterFullText READ masterFullText NOTIFY masterTimeChanged)

    Q_PROPERTY(QString layoutMode READ layoutMode WRITE setLayoutMode NOTIFY layoutChanged)
    Q_PROPERTY(int focusedStream READ focusedStream WRITE setFocusedStream NOTIFY layoutChanged)
    Q_PROPERTY(int audioStreamIndex READ audioStreamIndex NOTIFY audioChanged)
    Q_PROPERTY(bool timelineExpanded READ timelineExpanded WRITE setTimelineExpanded NOTIFY layoutChanged)
    Q_PROPERTY(bool diagnosticsVisible READ diagnosticsVisible WRITE setDiagnosticsVisible NOTIFY layoutChanged)
    Q_PROPERTY(bool dateNavVisible READ dateNavVisible WRITE setDateNavVisible NOTIFY layoutChanged)

    Q_PROPERTY(bool scanning READ scanning NOTIFY scanChanged)
    Q_PROPERTY(QString scanStatusText READ scanStatusText NOTIFY scanChanged)
    Q_PROPERTY(int scanTotalFiles READ scanTotalFiles NOTIFY scanChanged)
    Q_PROPERTY(int scanProbedFiles READ scanProbedFiles NOTIFY scanChanged)

    Q_PROPERTY(QObject* timeline READ timeline CONSTANT)
    Q_PROPERTY(QObject* dateNav READ dateNav CONSTANT)
    Q_PROPERTY(QObject* bookmarks READ bookmarks CONSTANT)
    Q_PROPERTY(QObject* diagnostics READ diagnostics CONSTANT)
    Q_PROPERTY(QObject* health READ health CONSTANT)

    // 音频监听与实时电平属性
    Q_PROPERTY(double audioVolume READ audioVolume NOTIFY audioChanged)
    Q_PROPERTY(bool audioMuted READ audioMuted NOTIFY audioChanged)
    Q_PROPERTY(bool audioFollowsFocus READ audioFollowsFocus NOTIFY settingsChanged)
    Q_PROPERTY(QString activeAudioName READ activeAudioName NOTIFY audioChanged)
    Q_PROPERTY(bool activeAudioHasTrack READ activeAudioHasTrack NOTIFY audioMeterChanged)
    Q_PROPERTY(int audioMeterChannels READ audioMeterChannels NOTIFY audioMeterChanged)
    Q_PROPERTY(double audioRmsNormL READ audioRmsNormL NOTIFY audioMeterChanged)
    Q_PROPERTY(double audioRmsNormR READ audioRmsNormR NOTIFY audioMeterChanged)
    Q_PROPERTY(double audioPeakHoldL READ audioPeakHoldL NOTIFY audioMeterChanged)
    Q_PROPERTY(double audioPeakHoldR READ audioPeakHoldR NOTIFY audioMeterChanged)
    Q_PROPERTY(QString audioPeakDbText READ audioPeakDbText NOTIFY audioMeterChanged)
    Q_PROPERTY(bool audioSignalActive READ audioSignalActive NOTIFY audioMeterChanged)
    Q_PROPERTY(QVariantList audioSourceList READ audioSourceList NOTIFY audioChanged)

    // 健康监测配置属性
    Q_PROPERTY(bool healthAutoCheck READ healthAutoCheck NOTIFY settingsChanged)
    Q_PROPERTY(int healthCheckIntervalMin READ healthCheckIntervalMin NOTIFY settingsChanged)
    Q_PROPERTY(bool healthNotifyOffline READ healthNotifyOffline NOTIFY settingsChanged)
    Q_PROPERTY(bool healthNotifyRecovered READ healthNotifyRecovered NOTIFY settingsChanged)

    Q_PROPERTY(bool developerDiagnostics READ developerDiagnostics WRITE setDeveloperDiagnostics NOTIFY settingsChanged)
    Q_PROPERTY(QString cacheDir READ cacheDir NOTIFY settingsChanged)
    Q_PROPERTY(int cacheMaxGb READ cacheMaxGb NOTIFY settingsChanged)
    Q_PROPERTY(bool preferHwDecode READ preferHwDecode NOTIFY settingsChanged)
    Q_PROPERTY(QVariantList cameraConfigs READ cameraConfigs NOTIFY settingsChanged)
    Q_PROPERTY(QVariantList streamConfigs READ streamConfigs NOTIFY settingsChanged)
    Q_PROPERTY(bool groupedLayoutAvailable READ groupedLayoutAvailable NOTIFY settingsChanged)
    Q_PROPERTY(int streamCount READ streamCount NOTIFY settingsChanged)

    // 快捷键属性
    Q_PROPERTY(QString shortcutTogglePlay READ shortcutTogglePlay NOTIFY settingsChanged)
    Q_PROPERTY(QString shortcutAddBookmark READ shortcutAddBookmark NOTIFY settingsChanged)
    Q_PROPERTY(QString shortcutStepBack READ shortcutStepBack NOTIFY settingsChanged)
    Q_PROPERTY(QString shortcutStepForward READ shortcutStepForward NOTIFY settingsChanged)
    Q_PROPERTY(QString shortcutExitFocus READ shortcutExitFocus NOTIFY settingsChanged)

    // 悬停高亮与摄像头所在地（日出日落昼夜计算）
    Q_PROPERTY(QString highlightedCameraId READ highlightedCameraId WRITE setHighlightedCameraId NOTIFY highlightChanged)
    Q_PROPERTY(bool locationConfigured READ locationConfigured NOTIFY settingsChanged)
    Q_PROPERTY(QString locationCity READ locationCity NOTIFY settingsChanged)
    Q_PROPERTY(double latitude READ latitude NOTIFY settingsChanged)
    Q_PROPERTY(double longitude READ longitude NOTIFY settingsChanged)
    Q_PROPERTY(int timezoneHours READ timezoneHours NOTIFY settingsChanged)
    Q_PROPERTY(QVariantList presetCities READ presetCities CONSTANT)
    Q_PROPERTY(QString timelineDayColor READ timelineDayColor NOTIFY settingsChanged)
    Q_PROPERTY(QString timelineNightColor READ timelineNightColor NOTIFY settingsChanged)

public:
    explicit PlaybackController(QObject* parent = nullptr);
    ~PlaybackController() override;

    void initialize();

    // 由 VideoRenderItem 或 QML 绑定注册
    Q_INVOKABLE void registerVideoItem(int streamIndex, QObject* item);
    Q_INVOKABLE void unregisterVideoItem(int streamIndex, QObject* item);

    bool playing() const;
    bool scrubbing() const;
    double playbackRate() const;
    qint64 masterTimeMs() const;
    QString masterDateText() const;
    QString masterTimeText() const;
    QString masterFullText() const;

    QString layoutMode() const { return m_config.lastLayoutMode; }
    void setLayoutMode(const QString& mode);

    int focusedStream() const { return m_config.lastFocusedStream; }
    void setFocusedStream(int idx); // -1=多路, 0..5=单路放大, -2=双摄整组放大

    int audioStreamIndex() const;
    bool timelineExpanded() const { return m_config.timelineExpanded; }
    void setTimelineExpanded(bool v);

    bool diagnosticsVisible() const { return m_config.diagnosticsVisible; }
    void setDiagnosticsVisible(bool v);

    bool dateNavVisible() const { return m_dateNavVisible; }
    void setDateNavVisible(bool v);

    bool scanning() const { return m_scanProgress.scanning || m_scanProgress.stageBRunning; }
    QString scanStatusText() const { return m_scanProgress.statusText; }
    int scanTotalFiles() const { return m_scanProgress.totalFiles; }
    int scanProbedFiles() const { return m_scanProgress.probedFiles; }

    QObject* timeline() const;
    QObject* dateNav() const;
    QObject* bookmarks() const;
    QObject* diagnostics() const;
    QObject* health() const;

    double audioVolume() const { return m_config.audioVolume; }
    bool audioMuted() const { return m_config.audioMuted; }
    bool audioFollowsFocus() const { return m_config.audioFollowsFocus; }
    QString activeAudioName() const;
    bool activeAudioHasTrack() const { return m_activeAudioHasTrack; }
    int audioMeterChannels() const { return m_meterChannels; }
    double audioRmsNormL() const { return m_rmsNormL; }
    double audioRmsNormR() const { return m_rmsNormR; }
    double audioPeakHoldL() const { return m_peakHoldL; }
    double audioPeakHoldR() const { return m_peakHoldR; }
    QString audioPeakDbText() const { return m_peakDbText; }
    bool audioSignalActive() const { return m_audioSignalActive; }
    QVariantList audioSourceList() const;

    bool healthAutoCheck() const { return m_config.healthAutoCheck; }
    int healthCheckIntervalMin() const { return m_config.healthCheckIntervalMin; }
    bool healthNotifyOffline() const { return m_config.healthNotifyOffline; }
    bool healthNotifyRecovered() const { return m_config.healthNotifyRecovered; }

    bool developerDiagnostics() const { return m_config.developerDiagnostics; }
    void setDeveloperDiagnostics(bool enabled);
    QString cacheDir() const { return m_config.cacheDir; }
    int cacheMaxGb() const { return m_config.cacheMaxGb; }
    bool preferHwDecode() const { return m_config.preferHwDecode; }
    QVariantList cameraConfigs() const;
    QVariantList streamConfigs() const;
    bool groupedLayoutAvailable() const;
    int streamCount() const { return m_config.streams.size(); }

    QString shortcutTogglePlay() const { return m_config.shortcuts.togglePlay; }
    QString shortcutAddBookmark() const { return m_config.shortcuts.addBookmark; }
    QString shortcutStepBack() const { return m_config.shortcuts.stepBackward; }
    QString shortcutStepForward() const { return m_config.shortcuts.stepForward; }
    QString shortcutExitFocus() const { return m_config.shortcuts.exitFocus; }

    QString highlightedCameraId() const { return m_highlightedCameraId; }
    Q_INVOKABLE void setHighlightedCameraId(const QString& camId);

    bool locationConfigured() const { return m_config.locationConfigured; }
    QString locationCity() const { return m_config.locationCity; }
    double latitude() const { return m_config.latitude; }
    double longitude() const { return m_config.longitude; }
    int timezoneHours() const { return m_config.timezoneOffsetSec / 3600; }
    QVariantList presetCities() const;
    QString timelineDayColor() const { return m_config.timelineDayColor; }
    QString timelineNightColor() const { return m_config.timelineNightColor; }

    // 播放控制指令
    Q_INVOKABLE void togglePlay();
    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void setPlaybackRate(double rate);
    Q_INVOKABLE void seekToMs(qint64 wallTimeMs);
    Q_INVOKABLE QString formatEpochMs(qint64 ms) const;
    Q_INVOKABLE qint64 parseJump(const QString& text) const;
    Q_INVOKABLE bool jumpToDateTimeString(const QString& text);
    Q_INVOKABLE void jumpToDateKeepTime(const QString& yyyyMmDd, bool exactOnly = false);
    Q_INVOKABLE void jumpToDateAtFraction(const QString& yyyyMmDd, double dayFraction);
    Q_INVOKABLE void jumpToToday(bool exactOnly = false);
    Q_INVOKABLE void jumpToYesterday(bool exactOnly = false);
    Q_INVOKABLE QVariantMap queryStreamNoVideoNeighbors(int streamIndex) const;
    Q_INVOKABLE void jumpStreamPrevSegment(int streamIndex);
    Q_INVOKABLE void jumpStreamNextSegment(int streamIndex);
    Q_INVOKABLE void stepSeconds(int deltaSec);
    Q_INVOKABLE void nextSegment();
    Q_INVOKABLE void prevSegment();

    // 穿梭与交互
    Q_INVOKABLE void handleWheelScrub(bool towardPast, int steps);
    Q_INVOKABLE void beginTimelineScrub();
    Q_INVOKABLE void updateTimelineScrub(qint64 targetMs);
    Q_INVOKABLE void endTimelineScrub();
    Q_INVOKABLE void toggleAudioStream(int streamIndex);
    Q_INVOKABLE void selectManualAudioStream(int streamIndex);
    Q_INVOKABLE void setAudioVolume(double vol);
    Q_INVOKABLE void toggleAudioMute();
    Q_INVOKABLE void resetAudioVolume100();
    Q_INVOKABLE void setAudioFollowsFocus(bool enabled);

    // 索引、局部复查、健康联动、OCR 校准、书签截图与设置
    Q_INVOKABLE void rescanDirectories();
    Q_INVOKABLE void recheckStreamAtCurrentTime(int streamIndex);
    Q_INVOKABLE void jumpToHealthLastVideo(qint64 jumpMs);
    Q_INVOKABLE void updateHealthSettings(bool autoCheck, int intervalMin,
                                            bool notifyOffline, bool notifyRecovered);
    Q_INVOKABLE void updateStreamNames(const QVariantList& names);
    Q_INVOKABLE QVariantList runAutoOcrCalibration(bool apply);
    Q_INVOKABLE void setStreamOffsetMs(int streamIndex, qint64 offsetMs);
    Q_INVOKABLE QString captureFrameScreenshot(int streamIndex);
    Q_INVOKABLE void addBookmarkWithCapture(const QString& title, const QString& note,
                                            const QString& color, int streamIndex);
    Q_INVOKABLE void updateCacheAndDecodeSettings(const QString& cacheDir,
                                                  int cacheMaxGb,
                                                  bool preferHw);
    Q_INVOKABLE void updateCameraDirectories(const QVariantList& paths, bool rescanNow);
    Q_INVOKABLE QString localFolderPath(const QUrl& folder) const { return folder.toLocalFile(); }
    Q_INVOKABLE bool detectDualCameraDir(const QString& rootPath) const;
    Q_INVOKABLE bool addCamera(const QString& displayName, const QString& rootPath, bool isDual);
    Q_INVOKABLE bool removeCamera(const QString& cameraId);
    Q_INVOKABLE QString normalizedShortcut(const QString& sequence) const;
    Q_INVOKABLE void updateShortcuts(const QString& togglePlay, const QString& addBookmark,
                                     const QString& stepBack, const QString& stepForward,
                                     const QString& exitFocus);
    Q_INVOKABLE void updateLocationSettings(bool configured, const QString& city,
                                            double lat, double lon, int tzHours);
    Q_INVOKABLE void updateTimelineColors(const QString& dayColor, const QString& nightColor);
    Q_INVOKABLE void clearAllCaches();
    Q_INVOKABLE void saveState();

signals:
    void clockStateChanged();
    void masterTimeChanged();
    void layoutChanged();
    void audioChanged();
    void audioMeterChanged();
    void scanChanged();
    void settingsChanged();
    void highlightChanged();
    void toastMessage(const QString& text, bool isError);

private slots:
    void onStageACompleted(int total, int changed, int removed);
    void onStageBCompleted(int probed, int corrupt);
    void onCameraRescanCompleted(int streamIndex, int changedCount);
    void onAudioMeterTick();

private:
    void pickInitialPlaybackTime();
    void reloadCameraConfiguration();
    void applyAudioRoutingForFocus(int focusedIdx);

    AppConfig m_config;
    std::unique_ptr<IndexDatabase> m_db;
    std::unique_ptr<NasBlockCache> m_cache;
    std::unique_ptr<FileScanner> m_scanner;
    std::unique_ptr<SyncScheduler> m_scheduler;
    std::unique_ptr<HealthMonitor> m_healthMonitor;
    TimelineModel* m_timeline = nullptr;
    DateNavModel* m_dateNav = nullptr;
    BookmarkModel* m_bookmarks = nullptr;
    DiagnosticsModel* m_diagnostics = nullptr;
    HealthModel* m_healthModel = nullptr;

    QVector<QPointer<VideoRenderItem>> m_videoItems;
    ScanProgress m_scanProgress;
    QTimer* m_autoSaveTimer = nullptr;
    QTimer* m_meterTimer = nullptr;
    bool m_initialSeekDone = false;
    bool m_dateNavVisible = true;
    QString m_highlightedCameraId;
    QHash<int, int64_t> m_lastAutoRescanMs;

    bool m_activeAudioHasTrack = false;
    int m_meterChannels = 1;
    double m_rmsNormL = 0.0;
    double m_rmsNormR = 0.0;
    double m_peakHoldL = 0.0;
    double m_peakHoldR = 0.0;
    QString m_peakDbText = QStringLiteral("-∞ dB");
    bool m_audioSignalActive = false;
};

} // namespace mcs
