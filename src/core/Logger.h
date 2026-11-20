#pragma once

#include <QFile>
#include <QMutex>
#include <QString>
#include <QStringList>
#include <deque>

namespace mcs {

enum class LogLevel {
    Debug = 0,
    Info = 1,
    Warn = 2,
    Error = 3
};

class Logger {
public:
    static Logger& instance();

    void init(const QString& logDir);
    void log(LogLevel level, const QString& category, const QString& message);
    QStringList recentLines(int maxCount = 200) const;
    QString logFilePath() const;

private:
    Logger() = default;
    ~Logger();

    mutable QMutex m_mutex;
    QFile m_file;
    QString m_logPath;
    std::deque<QString> m_ring;
    static constexpr size_t kMaxRingLines = 1000;
};

#define MCS_LOG_INFO(cat, msg)  ::mcs::Logger::instance().log(::mcs::LogLevel::Info, QStringLiteral(cat), (msg))
#define MCS_LOG_WARN(cat, msg)  ::mcs::Logger::instance().log(::mcs::LogLevel::Warn, QStringLiteral(cat), (msg))
#define MCS_LOG_ERROR(cat, msg) ::mcs::Logger::instance().log(::mcs::LogLevel::Error, QStringLiteral(cat), (msg))

} // namespace mcs
