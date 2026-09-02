/**
 * @file logger.h
 * @brief Thread-safe leveled logger with rotation.
 *
 * Thread safety: all public functions are safe to call concurrently from
 * any number of threads. Internally guarded by a single mutex; logging is
 * not on any hot path that requires lock-free behavior.
 */
#ifndef SM_LOGGER_H
#define SM_LOGGER_H

/* common.h must be included before any system header (it sets the POSIX
 * feature-test macro needed for pthread_rwlock_t under -std=c11; see the
 * comment at the top of common.h). */
#include "common.h"

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LOG_DEBUG = 0,
    LOG_INFO,
    LOG_WARNING,
    LOG_ERROR,
    LOG_FATAL
} sm_log_level_t;

/**
 * Initialize the logger.
 * @param path            Path to the log file. If NULL, logs go to stderr only.
 * @param min_level       Minimum level that will be emitted.
 * @param rotate_max_bytes Rotate (rename to .1) once the file exceeds this size.
 *                         0 disables rotation.
 * @return SM_OK or SM_ERR_IO if the file could not be opened.
 * Thread safety: call once at startup before other threads log.
 */
sm_status_t logger_init(const char *path, sm_log_level_t min_level, size_t rotate_max_bytes);

/** Flush and close the logger. Thread safety: call once at shutdown. */
void logger_shutdown(void);

/**
 * Emit a log line. Prefer the SM_LOG_* macros below rather than calling
 * this directly, so module/function/line are filled in automatically.
 * Thread safety: fully thread-safe, serialized internally.
 */
void logger_log(sm_log_level_t level, const char *module, const char *func,
                 int line, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 5, 6)))
#endif
    ;

#define SM_LOG_DEBUG(module, ...) \
    logger_log(LOG_DEBUG, (module), __func__, __LINE__, __VA_ARGS__)
#define SM_LOG_INFO(module, ...) \
    logger_log(LOG_INFO, (module), __func__, __LINE__, __VA_ARGS__)
#define SM_LOG_WARN(module, ...) \
    logger_log(LOG_WARNING, (module), __func__, __LINE__, __VA_ARGS__)
#define SM_LOG_ERROR(module, ...) \
    logger_log(LOG_ERROR, (module), __func__, __LINE__, __VA_ARGS__)
#define SM_LOG_FATAL(module, ...) \
    logger_log(LOG_FATAL, (module), __func__, __LINE__, __VA_ARGS__)

#ifdef __cplusplus
}
#endif
#endif /* SM_LOGGER_H */
