/** @file aigate_log.h
 *  @brief Leveled stderr logging used by all aigate modules.
 *
 *  Every call emits `<UTC ISO-8601> LEVEL message (file:line)` to stderr
 *  and flushes immediately. Thread-safe via an internal mutex.
 */
#ifndef AIGATE_LOG_H
#define AIGATE_LOG_H

#include <stdio.h>

/** @brief Emit a leveled log line to stderr.
 * @param level "INFO", "WARN", or "ERROR"
 * @param file  __FILE__ of the call site (passed by the macros)
 * @param line  __LINE__ of the call site (passed by the macros)
 * @param fmt   printf-style format; must not be NULL
 * @note Use the AIGATE_LOG_* macros; the function itself is not public API. */
void aigate_log(const char *level, const char *file, int line, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

#define AIGATE_LOG_INFO(...)  aigate_log("INFO",  __FILE__, __LINE__, __VA_ARGS__)
#define AIGATE_LOG_WARN(...)  aigate_log("WARN",  __FILE__, __LINE__, __VA_ARGS__)
#define AIGATE_LOG_ERROR(...) aigate_log("ERROR", __FILE__, __LINE__, __VA_ARGS__)

#endif /* AIGATE_LOG_H */
