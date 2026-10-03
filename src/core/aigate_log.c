/** @file aigate_log.c
 *  @brief Implementation of leveled and structured JSON/text stderr logger (see aigate_log.h). */
#include "aigate_log.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

typedef enum {
    AIGATE_LOG_LVL_DEBUG = 0,
    AIGATE_LOG_LVL_INFO = 1,
    AIGATE_LOG_LVL_WARN = 2,
    AIGATE_LOG_LVL_ERROR = 3
} aigate_log_level_t;

static aigate_log_level_t g_log_level = AIGATE_LOG_LVL_INFO;
static int                g_log_json = 0; /* 0: text, 1: json */

/** stderr log mutex (aigate_log holds lock across full path to prevent interleaving). */
static pthread_mutex_t g_log_mtx = PTHREAD_MUTEX_INITIALIZER;

static aigate_log_level_t
parse_level_str(const char* level)
{
    if (level == NULL) {
        return AIGATE_LOG_LVL_INFO;
    }
    if (strcasecmp(level, "DEBUG") == 0) {
        return AIGATE_LOG_LVL_DEBUG;
    }
    if (strcasecmp(level, "INFO") == 0) {
        return AIGATE_LOG_LVL_INFO;
    }
    if (strcasecmp(level, "WARN") == 0) {
        return AIGATE_LOG_LVL_WARN;
    }
    if (strcasecmp(level, "ERROR") == 0) {
        return AIGATE_LOG_LVL_ERROR;
    }
    return AIGATE_LOG_LVL_INFO;
}

void
aigate_log_init(const char* format, const char* level)
{
    pthread_mutex_lock(&g_log_mtx);
    if (format != NULL && strcasecmp(format, "json") == 0) {
        g_log_json = 1;
    } else {
        g_log_json = 0;
    }

    if (level != NULL) {
        g_log_level = parse_level_str(level);
    }
    pthread_mutex_unlock(&g_log_mtx);
}

/** @brief Helper to escape strings for JSON serialization into dst */
static void
json_escape_string(const char* src, char* dst, size_t dst_cap)
{
    if (dst == NULL || dst_cap == 0) {
        return;
    }
    dst[0] = '\0';
    if (src == NULL) {
        return;
    }
    size_t d = 0;
    for (size_t s = 0; src[s] != '\0' && d + 4 < dst_cap; s++) {
        unsigned char c = (unsigned char)src[s];
        if (c == '"') {
            dst[d++] = '\\';
            dst[d++] = '"';
        } else if (c == '\\') {
            dst[d++] = '\\';
            dst[d++] = '\\';
        } else if (c == '\n') {
            dst[d++] = '\\';
            dst[d++] = 'n';
        } else if (c == '\r') {
            dst[d++] = '\\';
            dst[d++] = 'r';
        } else if (c == '\t') {
            dst[d++] = '\\';
            dst[d++] = 't';
        } else if (c < 0x20) {
            int w = snprintf(dst + d, dst_cap - d, "\\u%04x", c);
            if (w > 0 && d + (size_t)w < dst_cap) {
                d += (size_t)w;
            }
        } else {
            dst[d++] = (char)c;
        }
    }
    dst[d] = '\0';
}

void
aigate_log(const char* level, const char* file, int line, const char* fmt, ...)
{
    aigate_log_level_t lvl = parse_level_str(level);
    if (lvl < g_log_level) {
        return;
    }

    char            ts[48];
    char            buf[32];
    struct timespec now;
    struct tm       tmv;
    va_list         ap;

    clock_gettime(CLOCK_REALTIME, &now);
    if (gmtime_r(&now.tv_sec, &tmv) != NULL &&
        strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S", &tmv) > 0) {
        snprintf(ts, sizeof ts, "%s.%03ld", buf, now.tv_nsec / 1000000L);
    } else {
        snprintf(ts, sizeof ts, "1970-01-01T00:00:00.000");
    }

    pthread_mutex_lock(&g_log_mtx);
    if (g_log_json) {
        char raw_msg[2048];
        char esc_msg[4096];
        char esc_file[256];
        va_start(ap, fmt);
        vsnprintf(raw_msg, sizeof raw_msg, fmt, ap);
        va_end(ap);
        json_escape_string(raw_msg, esc_msg, sizeof esc_msg);
        json_escape_string(file ? file : "?", esc_file, sizeof esc_file);
        fprintf(stderr,
                "{\"ts\":\"%s\",\"level\":\"%s\",\"file\":\"%s\",\"line\":%d,\"msg\":\"%s\"}\n",
                ts,
                level ? level : "INFO",
                esc_file,
                line,
                esc_msg);
    } else {
        fprintf(stderr, "%s %s ", ts, level ? level : "INFO");
        va_start(ap, fmt);
        vfprintf(stderr, fmt, ap);
        va_end(ap);
        fprintf(stderr, " (%s:%d)\n", file ? file : "?", line);
    }
    fflush(stderr);
    pthread_mutex_unlock(&g_log_mtx);
}
