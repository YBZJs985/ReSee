#pragma once

#include <QString>
#include <cstdint>
#include <optional>

namespace mcs::TimeUtils {

// 默认东八区 UTC+8 (28800 秒)，可通过配置显式调整，避免混用系统时区与相机本地时间
inline constexpr int DEFAULT_TZ_OFFSET_SEC = 8 * 3600;

struct ParsedFilename {
    QString channel;
    int64_t nominalStartMs = 0;
    int64_t nominalEndMs = 0;
};

// 解析小米录像文件名: <ch>_YYYYMMDDHHMMSS_YYYYMMDDHHMMSS.mp4
std::optional<ParsedFilename> parseXiaomiFilename(
    const QString& fileName,
    int tzOffsetSec = DEFAULT_TZ_OFFSET_SEC);

// 解析 "YYYYMMDDHHMMSS" 或 "YYYY-MM-DD HH:MM:SS" / "YYYY/MM/DD HH:MM:SS"
std::optional<int64_t> parseDateTimeString(
    const QString& text,
    int tzOffsetSec = DEFAULT_TZ_OFFSET_SEC);

// 将毫秒时间戳按显式时区格式化为 "YYYY-MM-DD HH:MM:SS"
QString formatEpochMs(
    int64_t epochMs,
    int tzOffsetSec = DEFAULT_TZ_OFFSET_SEC,
    bool withMillis = false);

// 格式化为时间点 "HH:MM:SS"
QString formatTimeOnlyMs(
    int64_t epochMs,
    int tzOffsetSec = DEFAULT_TZ_OFFSET_SEC);

// 格式化为日期 "YYYY-MM-DD"
QString formatDateOnlyMs(
    int64_t epochMs,
    int tzOffsetSec = DEFAULT_TZ_OFFSET_SEC);

// 返回指定毫秒时间戳所在日期的 00:00:00.000 毫秒数
int64_t dayStartMs(int64_t epochMs, int tzOffsetSec = DEFAULT_TZ_OFFSET_SEC);

// 当前真实系统时间 (Unix ms)
int64_t nowEpochMs();

// civil 年月日时分秒转 Unix 毫秒（无平台 localtime/mktime 锁）
int64_t civilToEpochMs(
    int year, int month, int day,
    int hour, int minute, int second,
    int ms = 0,
    int tzOffsetSec = DEFAULT_TZ_OFFSET_SEC);

void epochMsToCivil(
    int64_t epochMs,
    int& year, int& month, int& day,
    int& hour, int& minute, int& second, int& ms,
    int tzOffsetSec = DEFAULT_TZ_OFFSET_SEC);

} // namespace mcs::TimeUtils
