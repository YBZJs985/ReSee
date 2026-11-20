#pragma once

#include "core/AppConfig.h"
#include "core/Types.h"

#include <QObject>
#include <atomic>
#include <thread>

namespace mcs {

class IndexDatabase;
class NasBlockCache;

class FileScanner : public QObject {
    Q_OBJECT
public:
    explicit FileScanner(IndexDatabase* db, NasBlockCache* cache, QObject* parent = nullptr);
    ~FileScanner() override;

    void startScan(const AppConfig& config, bool runStageB = true);
    void startCameraRescan(const AppConfig& config, int streamIndex);
    void stop();
    void setForegroundBusy(bool busy);
    bool isStageARunning() const { return m_stageARunning.load(std::memory_order_acquire); }

    // 同步执行一次快速阶段 A（供 CLI 基准测试直接调用）
    bool runQuickScanBlocking(const AppConfig& config, int& outTotalFiles);

signals:
    void progressChanged(const mcs::ScanProgress& progress);
    void stageACompleted(int totalSegments, int newOrUpdated, int removed);
    void stageBCompleted(int probedCount, int corruptCount);
    void cameraRescanCompleted(int streamIndex, int changedCount);
    void scanError(const QString& message);

private:
    void workerMain(AppConfig config, bool runStageB);
    bool doStageA(const AppConfig& config, int& outTotal, int& outChanged, int& outRemoved);
    bool doCameraQuickScan(const AppConfig& config, int streamIndex, int& outChanged);
    void doStageB();

    IndexDatabase* m_db = nullptr;
    NasBlockCache* m_cache = nullptr;
    std::atomic<bool> m_stopRequested{false};
    std::atomic<bool> m_foregroundBusy{false};
    std::atomic<bool> m_stageARunning{false};
    std::jthread m_thread;
    std::jthread m_quickThread;
};

} // namespace mcs
