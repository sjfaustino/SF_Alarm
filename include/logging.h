#ifndef SF_ALARM_LOGGING_H
#define SF_ALARM_LOGGING_H

#include <Arduino.h>

// Log Levels
#define LOG_LEVEL_NONE  0
#define LOG_LEVEL_ERROR 1
#define LOG_LEVEL_WARN  2
#define LOG_LEVEL_INFO  3
#define LOG_LEVEL_DEBUG 4

// Standard compile-time limit
#ifndef CURRENT_LOG_LEVEL
#define CURRENT_LOG_LEVEL LOG_LEVEL_INFO
#endif

// Async Logging Interface (Iron Citadel)
void logInit();
void logPrintf(const char* level, const char* tag, const char* fmt, ...);

// Using macros to ensure zero overhead when disabled
#if CURRENT_LOG_LEVEL >= LOG_LEVEL_ERROR
#define LOG_ERROR(tag, fmt, ...) logPrintf("ERR", tag, fmt, ##__VA_ARGS__)
#else
#define LOG_ERROR(tag, fmt, ...)
#endif

#if CURRENT_LOG_LEVEL >= LOG_LEVEL_WARN
#define LOG_WARN(tag, fmt, ...)  logPrintf("WRN", tag, fmt, ##__VA_ARGS__)
#else
#define LOG_WARN(tag, fmt, ...)
#endif

#if CURRENT_LOG_LEVEL >= LOG_LEVEL_INFO
#define LOG_INFO(tag, fmt, ...)  logPrintf("INF", tag, fmt, ##__VA_ARGS__)
#else
#define LOG_INFO(tag, fmt, ...)
#endif

#if CURRENT_LOG_LEVEL >= LOG_LEVEL_DEBUG
#define LOG_DEBUG(tag, fmt, ...) logPrintf("DBG", tag, fmt, ##__VA_ARGS__)
#else
#define LOG_DEBUG(tag, fmt, ...)
#endif

#endif // SF_ALARM_LOGGING_H
