#include "core/Logger.h"
#include "core/TimeUtils.h"

#include <QDir>
#include <QTextStream>
#include <iostream>

namespace mcs {

Logger& Logger::instance() {
    static Logger inst;
    return inst;
}

Logger::~Logger() {
    QMutexLocker locker(&m_mutex);
    if (m_file.isOpen()) {
        m_file.flush();
        m_file.close();
    }
}

void Logger::init(const QString& logDir) {
    QMutexLocker locker(&m_mutex);
    QDir().mkpath(logDir);
    if (m_file.isOpen()) {
        m_file.close();
    }
    m_logPath = QDir(logDir).filePath(QStringLiteral("multicam_sync.log"));
    m_file.setFileName(m_logPath);
    // 超过 20MB 时轮转一次，避免日志无限增长
    if (m_file.exists() && m_file.size() > 20 * 1024 * 1024) {
        const QString oldPath = m_logPath + QStringLiteral(".1");
        QFile::remove(oldPath);
        QFile::rename(m_logPath, oldPath);
    }
    m_file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
}

void Logger::log(LogLevel level, const QString& category, const QString& message) {
    const char* lvlStr = "INFO";
    switch (level) {
    case LogLevel::Debug: lvlStr = "DBG"; break;
    case LogLevel::Info:  lvlStr = "INFO"; break;
    case LogLevel::Warn:  lvlStr = "WARN"; break;
    case LogLevel::Error: lvlStr = "ERR"; break;
    }

    const QString ts = TimeUtils::formatEpochMs(TimeUtils::nowEpochMs(), TimeUtils::DEFAULT_TZ_OFFSET_SEC, true);
    const QString line = QStringLiteral("[%1] [%2] [%3] %4").arg(ts, QLatin1String(lvlStr), category, message);

    QMutexLocker locker(&m_mutex);
    m_ring.push_back(line);
    while (m_ring.size() > kMaxRingLines) {
        m_ring.pop_front();
    }

    if (m_file.isOpen()) {
        QTextStream out(&m_file);
        out << line << '\n';
        if (level >= LogLevel::Warn) {
            out.flush();
        }
    }

    if (level >= LogLevel::Warn) {
        std::cerr << line.toUtf8().constData() << std::endl;
    }
}

QStringList Logger::recentLines(int maxCount) const {
    QMutexLocker locker(&m_mutex);
    QStringList res;
    int start = static_cast<int>(m_ring.size()) - maxCount;
    if (start < 0) start = 0;
    for (int i = start; i < static_cast<int>(m_ring.size()); ++i) {
        res.append(m_ring[i]);
    }
    return res;
}

QString Logger::logFilePath() const {
    QMutexLocker locker(&m_mutex);
    return m_logPath;
}

} // namespace mcs
