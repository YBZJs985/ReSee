#include "cache/NasBlockCache.h"
#include "core/Logger.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>

namespace mcs {

namespace {
constexpr size_t kInitialSidxReadBytes = 360448; // 352 KB 覆盖测试中所有 ftyp+moov+sidx (最大 347KB)

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
} // namespace

NasBlockCache::NasBlockCache() {
    m_prefetchThread = std::jthread([this]() { prefetchWorkerLoop(); });
}

NasBlockCache::~NasBlockCache() {
    m_prefetchStop.store(1ULL, std::memory_order_release);
    m_prefetchCv.notify_all();
    if (m_prefetchThread.joinable()) {
        m_prefetchThread.join();
    }
}

void NasBlockCache::configure(const QString& ssdCacheDir,
                              int ssdMaxGb,
                              int memMaxMb,
                              int maxConcurrentNasReads) {
    {
        std::lock_guard lk(m_memMtx);
        m_memMaxBytes = static_cast<uint64_t>(std::max(64, memMaxMb)) * 1024ULL * 1024ULL;
        while (m_memUsedBytes > m_memMaxBytes && !m_memLru.empty()) {
            const auto& back = m_memLru.back();
            m_memUsedBytes -= back.data->size();
            m_memMap.erase(back.key);
            m_memLru.pop_back();
        }
    }
    {
        std::lock_guard lk(m_nasSlotMtx);
        m_maxNasConcurrent = std::clamp(maxConcurrentNasReads, 1, 8);
    }

    bool needScanSsd = false;
    {
        std::lock_guard lk(m_ssdMtx);
        m_ssdMaxBytes = static_cast<uint64_t>(std::max(1, ssdMaxGb)) * 1024ULL * 1024ULL * 1024ULL;
        if (m_ssdDir != ssdCacheDir && !ssdCacheDir.isEmpty()) {
            m_ssdDir = ssdCacheDir;
            QDir().mkpath(m_ssdDir);
            needScanSsd = true;
        }
    }
    if (needScanSsd) {
        scanExistingSsdCache();
    }
}

void NasBlockCache::scanExistingSsdCache() {
    std::lock_guard lk(m_ssdMtx);
    m_ssdLru.clear();
    m_ssdMap.clear();
    m_ssdUsedBytes = 0;
    if (m_ssdDir.isEmpty()) return;

    struct EntryWithMtime {
        QString name;
        uint64_t bytes;
        long long mtime;
    };
    std::vector<EntryWithMtime> items;
    std::error_code ec;
    for (const auto& de : std::filesystem::directory_iterator(toFsPath(m_ssdDir), ec)) {
        if (!de.is_regular_file(ec)) continue;
        const auto name = fromFsPath(de.path().filename());
        if (!name.endsWith(QLatin1String(".blk"))) continue;
        const uint64_t sz = de.file_size(ec);
        const auto mt = de.last_write_time(ec).time_since_epoch().count();
        items.push_back({name, sz, static_cast<long long>(mt)});
    }
    // 按访问/修改时间倒序排列（最新在队头）
    std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) {
        return a.mtime > b.mtime;
    });

    for (const auto& it : items) {
        m_ssdLru.push_back({it.name, it.bytes});
        m_ssdMap[it.name] = std::prev(m_ssdLru.end());
        m_ssdUsedBytes += it.bytes;
    }

    while (m_ssdUsedBytes > m_ssdMaxBytes && !m_ssdLru.empty()) {
        const auto& back = m_ssdLru.back();
        std::filesystem::remove(toFsPath(m_ssdDir + QLatin1Char('/') + back.fileName), ec);
        m_ssdUsedBytes -= back.bytes;
        m_ssdMap.erase(back.fileName);
        m_ssdLru.pop_back();
    }
}

void NasBlockCache::clearAllCaches() {
    m_cacheEpoch.fetch_add(1, std::memory_order_acq_rel);
    {
        std::lock_guard lk(m_prefetchMtx);
        m_prefetchQueue.clear();
    }
    {
        std::lock_guard lk(m_memMtx);
        m_memLru.clear();
        m_memMap.clear();
        m_memUsedBytes = 0;
    }
    {
        std::lock_guard lk(m_sidxMtx);
        m_sidxLru.clear();
        m_sidxMap.clear();
    }
    {
        std::lock_guard lk(m_ssdMtx);
        std::error_code ec;
        for (const auto& entry : m_ssdLru) {
            std::filesystem::remove(toFsPath(m_ssdDir + QLatin1Char('/') + entry.fileName), ec);
        }
        m_ssdLru.clear();
        m_ssdMap.clear();
        m_ssdUsedBytes = 0;
    }
}

QString NasBlockCache::ssdChunkFileName(const ChunkKey& key) const {
    const QByteArray h = QCryptographicHash::hash(key.path.toUtf8(), QCryptographicHash::Sha1).toHex();
    return QString::fromLatin1(h.left(20)) + QStringLiteral("_%1.blk").arg(key.chunkIndex);
}

NasBlockCache::ChunkBytesPtr NasBlockCache::tryGetMem(const ChunkKey& key) {
    std::lock_guard lk(m_memMtx);
    auto it = m_memMap.find(key);
    if (it == m_memMap.end()) return nullptr;
    m_memLru.splice(m_memLru.begin(), m_memLru, it->second);
    return it->second->data;
}

void NasBlockCache::putMem(const ChunkKey& key, const ChunkBytesPtr& data) {
    if (!data || data->empty()) return;
    std::lock_guard lk(m_memMtx);
    auto it = m_memMap.find(key);
    if (it != m_memMap.end()) {
        m_memUsedBytes -= it->second->data->size();
        it->second->data = data;
        m_memUsedBytes += data->size();
        m_memLru.splice(m_memLru.begin(), m_memLru, it->second);
    } else {
        m_memLru.push_front({key, data});
        m_memMap[key] = m_memLru.begin();
        m_memUsedBytes += data->size();
    }

    while (m_memUsedBytes > m_memMaxBytes && !m_memLru.empty()) {
        const auto& back = m_memLru.back();
        m_memUsedBytes -= back.data->size();
        m_memMap.erase(back.key);
        m_memLru.pop_back();
    }
}

NasBlockCache::ChunkBytesPtr NasBlockCache::tryGetSsd(const QString& fileName) {
    QString fullPath;
    {
        std::lock_guard lk(m_ssdMtx);
        if (m_ssdDir.isEmpty()) return nullptr;
        auto it = m_ssdMap.find(fileName);
        if (it == m_ssdMap.end()) return nullptr;
        m_ssdLru.splice(m_ssdLru.begin(), m_ssdLru, it->second);
        fullPath = m_ssdDir + QLatin1Char('/') + fileName;
    }

    std::ifstream ifs(toFsPath(fullPath), std::ios::binary | std::ios::ate);
    if (!ifs.is_open()) {
        std::lock_guard lk(m_ssdMtx);
        auto it = m_ssdMap.find(fileName);
        if (it != m_ssdMap.end()) {
            m_ssdUsedBytes -= it->second->bytes;
            m_ssdLru.erase(it->second);
            m_ssdMap.erase(it);
        }
        return nullptr;
    }
    const auto sz = static_cast<size_t>(ifs.tellg());
    ifs.seekg(0, std::ios::beg);
    auto vec = std::make_shared<std::vector<uint8_t>>(sz);
    if (sz > 0) {
        ifs.read(reinterpret_cast<char*>(vec->data()), static_cast<std::streamsize>(sz));
        if (!ifs) return nullptr;
    }
    return vec;
}

void NasBlockCache::putSsd(const QString& fileName, const ChunkBytesPtr& data) {
    if (!data || data->empty()) return;
    QString dirCopy;
    {
        std::lock_guard lk(m_ssdMtx);
        if (m_ssdDir.isEmpty()) return;
        dirCopy = m_ssdDir;
    }

    const QString targetPath = dirCopy + QLatin1Char('/') + fileName;
    const QString tmpPath = targetPath + QStringLiteral(".tmp");
    {
        std::ofstream ofs(toFsPath(tmpPath), std::ios::binary | std::ios::trunc);
        if (!ofs.is_open()) return;
        ofs.write(reinterpret_cast<const char*>(data->data()), static_cast<std::streamsize>(data->size()));
        if (!ofs.good()) {
            ofs.close();
            std::error_code ec;
            std::filesystem::remove(toFsPath(tmpPath), ec);
            return;
        }
    }
    std::error_code ec;
    std::filesystem::rename(toFsPath(tmpPath), toFsPath(targetPath), ec);
    if (ec) {
        std::filesystem::remove(toFsPath(tmpPath), ec);
        return;
    }

    std::lock_guard lk(m_ssdMtx);
    auto it = m_ssdMap.find(fileName);
    if (it != m_ssdMap.end()) {
        m_ssdUsedBytes -= it->second->bytes;
        it->second->bytes = data->size();
        m_ssdUsedBytes += data->size();
        m_ssdLru.splice(m_ssdLru.begin(), m_ssdLru, it->second);
    } else {
        m_ssdLru.push_front({fileName, data->size()});
        m_ssdMap[fileName] = m_ssdLru.begin();
        m_ssdUsedBytes += data->size();
    }

    while (m_ssdUsedBytes > m_ssdMaxBytes && !m_ssdLru.empty()) {
        const auto& back = m_ssdLru.back();
        if (back.fileName == fileName) break;
        std::filesystem::remove(toFsPath(m_ssdDir + QLatin1Char('/') + back.fileName), ec);
        m_ssdUsedBytes -= back.bytes;
        m_ssdMap.erase(back.fileName);
        m_ssdLru.pop_back();
    }
}

bool NasBlockCache::acquireNasSlot(IoPriority priority,
                                   const std::atomic<uint64_t>* cancelToken,
                                   uint64_t expectedToken) {
    std::unique_lock lk(m_nasSlotMtx);
    if (priority == IoPriority::Foreground) {
        m_waitingForeground.fetch_add(1, std::memory_order_relaxed);
    } else if (priority == IoPriority::Prefetch) {
        ++m_waitingPrefetch;
    }

    auto cleanupWaiter = [&]() {
        if (priority == IoPriority::Foreground) {
            m_waitingForeground.fetch_sub(1, std::memory_order_relaxed);
        } else if (priority == IoPriority::Prefetch) {
            --m_waitingPrefetch;
        }
    };

    while (true) {
        if (cancelToken && cancelToken->load(std::memory_order_acquire) != expectedToken) {
            cleanupWaiter();
            return false;
        }

        const int limit = (priority == IoPriority::Foreground) ? m_maxNasConcurrent : std::min(2, m_maxNasConcurrent);
        const bool slotFree = (m_activeNasReads < limit);
        bool higherPriorityWaiting = false;
        if (priority == IoPriority::Prefetch && m_waitingForeground.load(std::memory_order_relaxed) > 0) {
            higherPriorityWaiting = true;
        } else if (priority == IoPriority::Background &&
                   (m_waitingForeground.load(std::memory_order_relaxed) > 0 || m_waitingPrefetch > 0)) {
            higherPriorityWaiting = true;
        }

        if (slotFree && !higherPriorityWaiting) {
            ++m_activeNasReads;
            cleanupWaiter();
            return true;
        }

        // 如果被取消，最多 10ms 醒来检查一次
        m_nasSlotCv.wait_for(lk, std::chrono::milliseconds(10));
    }
}

void NasBlockCache::releaseNasSlot() {
    {
        std::lock_guard lk(m_nasSlotMtx);
        if (m_activeNasReads > 0) --m_activeNasReads;
    }
    m_nasSlotCv.notify_all();
}

NasBlockCache::ChunkBytesPtr NasBlockCache::loadChunk(
    const QString& path,
    uint64_t fileSize,
    uint64_t chunkIndex,
    IoPriority priority,
    const std::atomic<uint64_t>* cancelToken,
    uint64_t expectedToken,
    bool* wasCancelled) {

    const ChunkKey key{path, chunkIndex};
    const QString ssdFile = ssdChunkFileName(key);

    for (int attempt = 0; attempt < 4; ++attempt) {
        if (cancelToken && cancelToken->load(std::memory_order_acquire) != expectedToken) {
            if (wasCancelled) *wasCancelled = true;
            return nullptr;
        }

        // 1. 内存 LRU
        if (auto mem = tryGetMem(key)) {
            std::lock_guard lk(m_statMtx);
            ++m_stats.memHits;
            return mem;
        }

        // 2. SSD LRU
        if (auto ssd = tryGetSsd(ssdFile)) {
            putMem(key, ssd);
            std::lock_guard lk(m_statMtx);
            ++m_stats.ssdHits;
            return ssd;
        }

        // 3. 在途请求合并（防止同块重复读 NAS）
        std::shared_ptr<InFlightRead> flight;
        bool isOwner = false;
        {
            std::lock_guard lk(m_inflightMtx);
            auto it = m_inflight.find(key);
            if (it != m_inflight.end()) {
                flight = it->second;
            } else {
                flight = std::make_shared<InFlightRead>();
                m_inflight.emplace(key, flight);
                isOwner = true;
            }
        }

        if (!isOwner) {
            bool needRetry = false;
            {
                std::unique_lock lk(flight->mtx);
                while (!flight->done) {
                    if (cancelToken && cancelToken->load(std::memory_order_acquire) != expectedToken) {
                        if (wasCancelled) *wasCancelled = true;
                        return nullptr;
                    }
                    flight->cv.wait_for(lk, std::chrono::milliseconds(10));
                }
                if (flight->ok) {
                    return flight->data;
                }
                needRetry = flight->ownerCancelled;
            }
            if (needRetry) {
                continue; // 之前的 owner 因自身被取消而退出，未被取消的等待者重新接管读取
            }
            return nullptr;
        }

        // 成为读取者：受并发槽与优先级限流保护
        const uint64_t epochBefore = m_cacheEpoch.load(std::memory_order_acquire);
        ChunkBytesPtr loadedData;
        bool ok = false;
        bool ownerWasCancelled = false;

        if (acquireNasSlot(priority, cancelToken, expectedToken)) {
            const auto t0 = std::chrono::steady_clock::now();
            std::error_code ec;
            const auto fsPath = toFsPath(path);
            uint64_t actualSize = fileSize;
            if (actualSize == 0) {
                actualSize = std::filesystem::file_size(fsPath, ec);
            }
            const uint64_t chunkStart = chunkIndex * kChunkSize;
            if (!ec && chunkStart < actualSize) {
                const size_t toRead = static_cast<size_t>(std::min<uint64_t>(kChunkSize, actualSize - chunkStart));
                std::ifstream ifs(fsPath, std::ios::binary);
                if (ifs.is_open()) {
                    ifs.seekg(static_cast<std::streamoff>(chunkStart), std::ios::beg);
                    auto vec = std::make_shared<std::vector<uint8_t>>(toRead);
                    size_t totalGot = 0;
                    // 分 256KB 小片读取，允许在途快速响应 Seek 取消
                    while (totalGot < toRead) {
                        if (cancelToken && cancelToken->load(std::memory_order_acquire) != expectedToken) {
                            ownerWasCancelled = true;
                            break;
                        }
                        const size_t step = std::min<size_t>(262144, toRead - totalGot);
                        ifs.read(reinterpret_cast<char*>(vec->data() + totalGot), static_cast<std::streamsize>(step));
                        const std::streamsize got = ifs.gcount();
                        if (got <= 0) break;
                        totalGot += static_cast<size_t>(got);
                        if (static_cast<size_t>(got) < step) break;
                    }
                    if (!ownerWasCancelled && totalGot > 0) {
                        vec->resize(totalGot);
                        loadedData = vec;
                        ok = true;
                    }
                }
            }
            const double elapsedMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0).count();
            releaseNasSlot();

            if (ok && m_cacheEpoch.load(std::memory_order_acquire) == epochBefore) {
                putMem(key, loadedData);
                putSsd(ssdFile, loadedData);
                std::lock_guard lk(m_statMtx);
                ++m_stats.nasReads;
                m_stats.nasBytesRead += loadedData->size();
                m_stats.lastNasReadMs = elapsedMs;
                ++m_nasReadCountForAvg;
                m_totalNasReadMs += elapsedMs;
                m_stats.avgNasReadMs = m_totalNasReadMs / static_cast<double>(m_nasReadCountForAvg);
                m_stats.nasDisconnected = false;
                m_stats.lastError.clear();
            } else if (!ownerWasCancelled && !ok) {
                std::lock_guard lk(m_statMtx);
                m_stats.nasDisconnected = !std::filesystem::exists(fsPath, ec);
                m_stats.lastError = QStringLiteral("读取失败: %1").arg(path);
            }
        } else {
            ownerWasCancelled = true;
        }

        if (ownerWasCancelled && wasCancelled) {
            *wasCancelled = true;
        }

        {
            std::lock_guard lk(m_inflightMtx);
            m_inflight.erase(key);
        }
        {
            std::lock_guard lk(flight->mtx);
            flight->data = loadedData;
            flight->ok = ok;
            flight->ownerCancelled = ownerWasCancelled;
            flight->done = true;
        }
        flight->cv.notify_all();
        return loadedData;
    }
    return nullptr;
}

bool NasBlockCache::readRange(const QString& path,
                              uint64_t fileSize,
                              uint64_t offset,
                              size_t size,
                              std::vector<uint8_t>& outBuf,
                              IoPriority priority,
                              const std::atomic<uint64_t>* cancelToken,
                              uint64_t expectedToken,
                              bool* wasCancelled) {
    if (wasCancelled) *wasCancelled = false;
    outBuf.clear();
    if (size == 0 || (fileSize > 0 && offset >= fileSize)) return false;

    const uint64_t endOffset = (fileSize > 0)
        ? std::min<uint64_t>(offset + size, fileSize)
        : (offset + size);
    const size_t totalNeeded = static_cast<size_t>(endOffset - offset);
    outBuf.resize(totalNeeded);

    size_t copied = 0;
    uint64_t cur = offset;
    while (cur < endOffset) {
        if (cancelToken && cancelToken->load(std::memory_order_acquire) != expectedToken) {
            if (wasCancelled) *wasCancelled = true;
            outBuf.clear();
            return false;
        }
        const uint64_t chunkIdx = cur / kChunkSize;
        const size_t inChunkOffset = static_cast<size_t>(cur % kChunkSize);
        bool cancelledChunk = false;
        const ChunkBytesPtr chunk = loadChunk(
            path, fileSize, chunkIdx, priority, cancelToken, expectedToken, &cancelledChunk);
        if (cancelledChunk) {
            if (wasCancelled) *wasCancelled = true;
            outBuf.clear();
            return false;
        }
        if (!chunk || inChunkOffset >= chunk->size()) {
            if (copied > 0) {
                outBuf.resize(copied);
                return true;
            }
            outBuf.clear();
            return false;
        }
        const size_t avail = chunk->size() - inChunkOffset;
        const size_t toCopy = std::min(avail, totalNeeded - copied);
        std::memcpy(outBuf.data() + copied, chunk->data() + inChunkOffset, toCopy);
        copied += toCopy;
        cur += toCopy;
    }
    return copied > 0;
}

std::shared_ptr<const SidxIndex> NasBlockCache::getOrLoadSidx(
    const QString& path,
    uint64_t fileSize,
    IoPriority priority,
    const std::atomic<uint64_t>* cancelToken,
    uint64_t expectedToken) {
    {
        std::lock_guard lk(m_sidxMtx);
        auto it = m_sidxMap.find(path);
        if (it != m_sidxMap.end()) {
            m_sidxLru.splice(m_sidxLru.begin(), m_sidxLru, it->second);
            return it->second->second;
        }
    }

    uint64_t actualSize = fileSize;
    if (actualSize == 0) {
        std::error_code ec;
        actualSize = std::filesystem::file_size(toFsPath(path), ec);
        if (ec || actualSize < 16) return nullptr;
    }

    std::vector<uint8_t> hdrBuf;
    const size_t firstRead = static_cast<size_t>(std::min<uint64_t>(kInitialSidxReadBytes, actualSize));
    if (!readRange(path, actualSize, 0, firstRead, hdrBuf, priority, cancelToken, expectedToken)) {
        return nullptr;
    }

    SidxIndex parsed = SidxParser::parseHeaderBytes(hdrBuf.data(), hdrBuf.size(), actualSize);
    if (parsed.needMoreHeader && parsed.suggestedHeaderBytes > hdrBuf.size()) {
        const size_t extRead = static_cast<size_t>(std::min<uint64_t>(parsed.suggestedHeaderBytes, actualSize));
        if (readRange(path, actualSize, 0, extRead, hdrBuf, priority, cancelToken, expectedToken)) {
            parsed = SidxParser::parseHeaderBytes(hdrBuf.data(), hdrBuf.size(), actualSize);
        }
    }

    auto ptr = std::make_shared<SidxIndex>(std::move(parsed));
    {
        std::lock_guard lk(m_sidxMtx);
        auto it = m_sidxMap.find(path);
        if (it != m_sidxMap.end()) {
            it->second->second = ptr;
            m_sidxLru.splice(m_sidxLru.begin(), m_sidxLru, it->second);
        } else {
            m_sidxLru.emplace_front(path, ptr);
            m_sidxMap[path] = m_sidxLru.begin();
            while (m_sidxLru.size() > kMaxSidxCacheEntries) {
                const QString oldKey = m_sidxLru.back().first;
                m_sidxMap.erase(oldKey);
                m_sidxLru.pop_back();
            }
        }
    }
    return ptr;
}

void NasBlockCache::enqueuePrefetch(const QString& path, uint64_t fileSize, uint64_t offset, size_t size) {
    {
        std::lock_guard lk(m_prefetchMtx);
        for (const auto& req : m_prefetchQueue) {
            if (req.path == path && req.offset == offset) return;
        }
        if (m_prefetchQueue.size() >= 16) {
            m_prefetchQueue.pop_front();
        }
        m_prefetchQueue.push_back({path, fileSize, offset, size});
    }
    m_prefetchCv.notify_one();
}

void NasBlockCache::prefetchWorkerLoop() {
    while (m_prefetchStop.load(std::memory_order_acquire) == 0ULL) {
        PrefetchReq req;
        {
            std::unique_lock lk(m_prefetchMtx);
            m_prefetchCv.wait(lk, [this]() {
                return m_prefetchStop.load(std::memory_order_acquire) != 0ULL || !m_prefetchQueue.empty();
            });
            if (m_prefetchStop.load(std::memory_order_acquire) != 0ULL) break;
            req = std::move(m_prefetchQueue.front());
            m_prefetchQueue.pop_front();
        }
        std::vector<uint8_t> dummy;
        readRange(req.path, req.fileSize, req.offset, req.size, dummy,
                  IoPriority::Prefetch, &m_prefetchStop, 0ULL, nullptr);
    }
}

void NasBlockCache::prefetchRange(const QString& path, uint64_t fileSize, uint64_t offset, size_t size) {
    std::vector<uint8_t> dummy;
    readRange(path, fileSize, offset, size, dummy, IoPriority::Prefetch, nullptr, 0, nullptr);
}

int NasBlockCache::activeForegroundCount() const {
    return m_waitingForeground.load(std::memory_order_relaxed);
}

CacheStats NasBlockCache::stats() const {
    CacheStats st;
    {
        std::lock_guard lk(m_statMtx);
        st = m_stats;
    }
    {
        std::lock_guard lk(m_memMtx);
        st.memBytesUsed = m_memUsedBytes;
        st.memMaxBytes = m_memMaxBytes;
    }
    {
        std::lock_guard lk(m_ssdMtx);
        st.ssdBytesUsed = m_ssdUsedBytes;
        st.ssdMaxBytes = m_ssdMaxBytes;
    }
    return st;
}

bool NasBlockCache::isFileReachable(const QString& path) const {
    std::error_code ec;
    return std::filesystem::exists(toFsPath(path), ec);
}

} // namespace mcs
