/** @file aigate_log.c
 *  @brief Implementation of the leveled stderr logger (see aigate_log.h). */
#include "aigate_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <pthread.h>
#include <time.h>

static pthread_mutex_t g_log_mtx = PTHREAD_MUTEX_INITIALIZER;

void aigate_log(const char *level, const char *file, int line, const char *fmt, ...)
{
  char ts[48];
  char buf[32];
  struct timespec now;
  struct tm tmv;
  va_list ap;

  clock_gettime(CLOCK_REALTIME, &now);
  if (gmtime_r(&now.tv_sec, &tmv) != NULL &&
      strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S", &tmv) > 0)
    snprintf(ts, sizeof ts, "%s.%03ld", buf, now.tv_nsec / 1000000L);
  else
    snprintf(ts, sizeof ts, "1970-01-01T00:00:00.000");

  pthread_mutex_lock(&g_log_mtx);
  fprintf(stderr, "%s %s ", ts, level ? level : "INFO");
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fprintf(stderr, " (%s:%d)\n", file ? file : "?", line);
  fflush(stderr);
  pthread_mutex_unlock(&g_log_mtx);
}
