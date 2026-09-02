/**
 * @file logger.c
 * @brief Implementation of the thread-safe leveled logger.
 */
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>

static FILE            *g_fp             = NULL;
static char              g_path[512]     = {0};
static sm_log_level_t    g_min_level     = LOG_INFO;
static size_t            g_rotate_max    = 0;
static sm_mutex_t   g_lock          = SM_MUTEX_INITIALIZER;
static bool              g_initialized   = false;

static const char *level_name(sm_log_level_t l)
{
    switch (l) {
        case LOG_DEBUG:   return "DEBUG";
        case LOG_INFO:    return "INFO";
        case LOG_WARNING: return "WARN";
        case LOG_ERROR:   return "ERROR";
        case LOG_FATAL:   return "FATAL";
        default:          return "UNKNOWN";
    }
}

const char *sm_status_str(sm_status_t st)
{
    switch (st) {
        case SM_OK:               return "OK";
        case SM_ERR_NULL_PARAM:   return "NULL_PARAM";
        case SM_ERR_ALLOC_FAILED: return "ALLOC_FAILED";
        case SM_ERR_NOT_FOUND:    return "NOT_FOUND";
        case SM_ERR_DUPLICATE:    return "DUPLICATE";
        case SM_ERR_INVALID_ARG:  return "INVALID_ARG";
        case SM_ERR_IO:           return "IO_ERROR";
        case SM_ERR_CORRUPT_DATA: return "CORRUPT_DATA";
        case SM_ERR_LOCK_FAILED:  return "LOCK_FAILED";
        case SM_ERR_CAPACITY:     return "CAPACITY";
        case SM_ERR_UNAUTHORIZED: return "UNAUTHORIZED";
        default:                  return "INTERNAL_ERROR";
    }
}

/* Single-threaded, portable build: there is exactly one thread of
 * execution, so an execution-id is always 0. Kept as a function (rather
 * than deleted) so the log line format and call sites stay unchanged. */
static long get_tid(void)
{
    return 0L;
}

/* Rotate the current log file to <path>.1 if it exceeds g_rotate_max bytes.
 * Caller must hold g_lock. */
static void maybe_rotate(void)
{
    if (g_rotate_max == 0U || g_fp == NULL || g_path[0] == '\0') {
        return;
    }
    long pos = ftell(g_fp);
    if (pos < 0 || (size_t)pos < g_rotate_max) {
        return;
    }
    fflush(g_fp);
    fclose(g_fp);

    char backup[600];
    (void)snprintf(backup, sizeof(backup), "%s.1", g_path);
    (void)rename(g_path, backup); /* best-effort; ignore failure */

    g_fp = fopen(g_path, "a");
    /* If reopening the file fails, later logging will fall back to stderr. */
}

sm_status_t logger_init(const char *path, sm_log_level_t min_level, size_t rotate_max_bytes)
{
    if (sm_mutex_lock(&g_lock) != 0) {
        return SM_ERR_LOCK_FAILED;
    }
    g_min_level  = min_level;
    g_rotate_max = rotate_max_bytes;

    if (path != NULL) {
        (void)strncpy(g_path, path, sizeof(g_path) - 1U);
        g_path[sizeof(g_path) - 1U] = '\0';
        g_fp = fopen(path, "a");
        if (g_fp == NULL) {
            (void)sm_mutex_unlock(&g_lock);
            return SM_ERR_IO;
        }
    } else {
        g_fp = NULL; /* stderr only */
    }
    g_initialized = true;
    (void)sm_mutex_unlock(&g_lock);
    return SM_OK;
}

void logger_shutdown(void)
{
    (void)sm_mutex_lock(&g_lock);
    if (g_fp != NULL) {
        fflush(g_fp);
        fclose(g_fp);
        g_fp = NULL;
    }
    g_initialized = false;
    (void)sm_mutex_unlock(&g_lock);
}

void logger_log(sm_log_level_t level, const char *module, const char *func,
                 int line, const char *fmt, ...)
{
    if (level < g_min_level) {
        return;
    }
    char timebuf[32];
    time_t now = time(NULL);
    /* gmtime() returns a pointer into a static, process-wide buffer that is
     * NOT thread-safe -- concurrent callers can race on it and corrupt each
     * other's timestamp. gmtime_r() writes into a caller-owned struct tm
     * instead, so each thread gets its own storage. Same output format,
     * just safe under real concurrent callers. */
    struct tm tm_storage;
    const struct tm *tm_info = gmtime_r(&now, &tm_storage);
    if (tm_info != NULL) {
        (void)strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", tm_info);
    } else {
        (void)snprintf(timebuf, sizeof(timebuf), "unknown-time");
    }

    char msg[1024];
    va_list args;
    va_start(args, fmt);
    (void)vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    if (sm_mutex_lock(&g_lock) != 0) {
        return; /* best-effort logging: never crash the caller */
    }

    maybe_rotate();

    FILE *out = (g_fp != NULL) ? g_fp : stderr;
    (void)fprintf(out, "%s [%-5s] [tid=%ld] [%s::%s:%d] %s\n",
                  timebuf, level_name(level),
                  get_tid(), module != NULL ? module : "?",
                  func != NULL ? func : "?", line, msg);
    if (g_fp != NULL) {
        fflush(g_fp); /* durability over raw throughput: acceptable for this scale */
    }

    (void)sm_mutex_unlock(&g_lock);

    if (level == LOG_FATAL) {
        /* Fatal conditions are surfaced but the process does NOT abort here;
         * callers decide how to react (this is a library, not an app). */
    }
}
