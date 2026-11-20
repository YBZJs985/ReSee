#pragma once

#include "index/SidxParser.h"

#include <QMutex>
#include <QString>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <list>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace mcs {

enum class IoPriority : int {
    Foreground = 0, // 实时播放与用户 Seek（最高优先级）
    Prefetch = 1,   // 切片预读
    Background = 2  // 后台 Stage B 索引探测（最低优先级）
};

struct CacheStats {
    uint64_t memHits = 0;
    uint64_t ssdHits = 0;
    uint64_t nasReads = 0;
    uint64_t nasBytesRead = 0;
    uint64_t ssdBytesUsed = 0;
    uint64_t ssdMaxBytes = 0;
    uint64_t memBytesUsed = 0;
    uint64_t memMaxBytes = 0;
    double avgNasReadMs = 0.0;
    double lastNasReadMs = 0.0;
    bool nasDisconnected = false;
    QString lastError;
};

class NasBlockCache {
public:
    static constexpr size_t kChunkSize = 1024 * 1024; // 1MB 对齐块，兼顾顺序吞吐与冷Seek低延迟

    NasBlockCache();
    ~NasBlockCache();

    void configure(const QString& ssdCacheDir,
                   int ssdMaxGb,
                   int memMaxMb,
                   int maxConcurrentNasReads);

    void clearAllCaches();

    // 读取任意范围 [offset, offset + size)，自动拆分为 2MB 对齐块并经 内存LRU -> SSD LRU -> NAS 限流读取
    // 若 cancelToken != nullptr 且 *cancelToken != expectedToken，立即中止返回 false
    bool readRange(const QString& path,
                   uint64_t fileSize,
                   uint64_t offset,
                   size_t size,
                   std::vector<uint8_t>& outBuf,
                   IoPriority priority = IoPriority::Foreground,
                   const std::atomic<uint64_t>* cancelToken = nullptr,
                   uint64_t expectedToken = 0,
                   bool* wasCancelled = nullptr);

    // 获取或解析文件头部 SIDX 索引（带内存索引缓存 + 分块底层读）
    std::shared_ptr<const SidxIndex> getOrLoadSidx(
        const QString& path,
        uint64_t fileSize,
        IoPriority priority = IoPriority::Foreground,
        const std::atomic<uint64_t>* cancelToken = nullptr,
        uint64_t expectedToken = 0);

    // 异步预取文件头部或指定偏移块（在受控预读线程中执行，生命周期随缓存安全结束）
    void enqueuePrefetch(const QString& path, uint64_t fileSize, uint64_t offset, size_t size);
    void prefetchRange(const QString& path, uint64_t fileSize, uint64_t offset, size_t size);

    int activeForegroundCount() const;
    CacheStats stats() const;
    bool isFileReachable(const QString& path) const;

private:
    struct ChunkKey {
        QString path;
        uint64_t chunkIndex = 0;
        bool operator==(const ChunkKey& o) const {
            return chunkIndex == o.chunkIndex && path == o.path;
        }
    };

    struct ChunkKeyHash {
        size_t operator()(const ChunkKey& k) const noexcept {
            const size_t h1 = std::hash<std::u16string_view>{}(
                std::u16string_view(reinterpret_cast<const char16_t*>(k.path.constData()),
                                    static_cast<size_t>(k.path.size())));
            const size_t h2 = std::hash<uint64_t>{}(k.chunkIndex);
            return h1 ^ (h2 * 0x9e3779b97f4a7c15ULL + 0x9e3779b9 + (h1 << 6) + (h1 >> 2));
        }
    };

    using ChunkBytesPtr = std::shared_ptr<const std::vector<uint8_t>>;

    struct MemLruEntry {
        ChunkKey key;
        ChunkBytesPtr data;
    };

    struct SsdLruEntry {
        QString fileName; // 缓存目录下相对文件名
        uint64_t bytes = 0;
    };

    struct InFlightRead {
        std::mutex mtx;
        std::condition_variable cv;
        bool done = false;
        bool ok = false;
        bool ownerCancelled = false;
        ChunkBytesPtr data;
    };

    ChunkBytesPtr loadChunk(const QString& path,
                            uint64_t fileSize,
                            uint64_t chunkIndex,
                            IoPriority priority,
                            const std::atomic<uint64_t>* cancelToken,
                            uint64_t expectedToken,
                            bool* wasCancelled);

    ChunkBytesPtr tryGetMem(const ChunkKey& key);
    void putMem(const ChunkKey& key, const ChunkBytesPtr& data);

    QString ssdChunkFileName(const ChunkKey& key) const;
    ChunkBytesPtr tryGetSsd(const QString& fileName);
    void putSsd(const QString& fileName, const ChunkBytesPtr& data);
    void scanExistingSsdCache();

    bool acquireNasSlot(IoPriority priority,
                        const std::atomic<uint64_t>* cancelToken,
                        uint64_t expectedToken);
    void releaseNasSlot();

    // 内存分块 LRU
    mutable std::mutex m_memMtx;
    std::list<MemLruEntry> m_memLru;
    std::unordered_map<ChunkKey, std::list<MemLruEntry>::iterator, ChunkKeyHash> m_memMap;
    uint64_t m_memUsedBytes = 0;
    uint64_t m_memMaxBytes = 512ULL * 1024 * 1024;

    // SSD 分块 LRU
    mutable std::mutex m_ssdMtx;
    QString m_ssdDir;
    std::list<SsdLruEntry> m_ssdLru;
    std::unordered_map<QString, std::list<SsdLruEntry>::iterator> m_ssdMap;
    uint64_t m_ssdUsedBytes = 0;
    uint64_t m_ssdMaxBytes = 15ULL * 1024 * 1024 * 1024;
    std::atomic<uint64_t> m_cacheEpoch{0};

    // SIDX 索引缓存
    mutable std::mutex m_sidxMtx;
    std::list<std::pair<QString, std::shared_ptr<const SidxIndex>>> m_sidxLru;
    std::unordered_map<QString, decltype(m_sidxLru)::iterator> m_sidxMap;
    static constexpr size_t kMaxSidxCacheEntries = 2048;

    // 在途请求去重
    std::mutex m_inflightMtx;
    std::unordered_map<ChunkKey, std::shared_ptr<InFlightRead>, ChunkKeyHash> m_inflight;

    // NAS 并发与优先级限流器
    mutable std::mutex m_nasSlotMtx;
    std::condition_variable m_nasSlotCv;
    int m_maxNasConcurrent = 3;
    int m_activeNasReads = 0;
    std::atomic<int> m_waitingForeground{0};
    int m_waitingPrefetch = 0;

    // 统计与状态
    mutable std::mutex m_statMtx;
    CacheStats m_stats;
    uint64_t m_nasReadCountForAvg = 0;
    double m_totalNasReadMs = 0.0;

    // 受控后台预读工作队列
    struct PrefetchReq {
        QString path;
        uint64_t fileSize = 0;
        uint64_t offset = 0;
        size_t size = 0;
    };
    void prefetchWorkerLoop();
    std::mutex m_prefetchMtx;
    std::condition_variable m_prefetchCv;
    std::deque<PrefetchReq> m_prefetchQueue;
    std::atomic<uint64_t> m_prefetchStop{0};
    std::jthread m_prefetchThread;
};

} // namespace mcs
