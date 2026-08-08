/**
 * @file common.h
 * @brief Common types, error codes, and macros shared across all modules.
 */
#ifndef SM_COMMON_H
#define SM_COMMON_H

/* pthread_rwlock_t and friends are POSIX (not ISO C), so under strict
 * -std=c11 glibc's <features.h> hides them unless a POSIX feature-test
 * macro is visible *before the first system header of any kind* pulls
 * <features.h> in (stdint.h/stddef.h/stdbool.h included below all do).
 * Defining it here, as literally the first thing in this file, restores
 * those declarations without relaxing -std=c11 anywhere else in the
 * build or touching the Makefile. */
#if defined(_WIN32) || defined(__unix__) || defined(__APPLE__)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#if defined(_WIN32) || defined(__unix__) || defined(__APPLE__)
#include <pthread.h>
#define SM_HAS_PTHREADS 1
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** Global status codes returned by nearly every API in this project. */
typedef enum {
    SM_OK                      = 0,
    SM_ERR_NULL_PARAM          = -1,
    SM_ERR_ALLOC_FAILED        = -2,
    SM_ERR_NOT_FOUND           = -3,
    SM_ERR_DUPLICATE           = -4,
    SM_ERR_INVALID_ARG         = -5,
    SM_ERR_IO                  = -6,
    SM_ERR_CORRUPT_DATA        = -7,
    SM_ERR_LOCK_FAILED         = -8,
    SM_ERR_CAPACITY            = -9,
    SM_ERR_UNAUTHORIZED        = -10,
    SM_ERR_INTERNAL            = -99
} sm_status_t;

/** Convert a status code to a human readable string (never returns NULL). */
const char *sm_status_str(sm_status_t st);

/* Bounds used by fixed-size fields. Chosen to match real 3GPP field widths. */
#define SM_MAX_IMSI_LEN     15u   /* 3GPP TS 23.003: IMSI <= 15 digits      */
#define SM_MAX_MSISDN_LEN   15u   /* E.164 <= 15 digits                     */
#define SM_MAX_NAME_LEN     64u
#define SM_MAX_REGION_LEN   32u
#define SM_MAX_PLAN_LEN     32u
#define SM_MAX_LINE_LEN     512u

/** Macro to silence unused-parameter warnings without hiding real bugs. */
#define SM_UNUSED(x) ((void)(x))

/* ------------------------------------------------------------------------
 * Portable synchronization shim.
 *
 * The original build relied on POSIX pthreads (pthread_mutex_t /
 * pthread_rwlock_t), which is Linux/POSIX-specific. This project now runs
 * single-threaded and is built with plain, standard C (C11, no platform
 * headers), so these become trivial no-op wrappers: they preserve every
 * call site's lock/unlock structure (and therefore its logic and MISRA
 * layout) without requiring any OS threading API. If multi-threading is
 * ever reintroduced, only this block needs to change.
 * ---------------------------------------------------------------------- */
#ifdef SM_HAS_PTHREADS
typedef pthread_mutex_t sm_mutex_t;
typedef pthread_rwlock_t sm_rwlock_t;
#define SM_MUTEX_INITIALIZER PTHREAD_MUTEX_INITIALIZER

static inline int sm_mutex_init(sm_mutex_t *m)
{
    if (m == NULL) { return -1; }
    return pthread_mutex_init(m, NULL);
}

static inline int sm_mutex_lock(sm_mutex_t *m)
{
    return (m == NULL) ? -1 : pthread_mutex_lock(m);
}

static inline int sm_mutex_unlock(sm_mutex_t *m)
{
    return (m == NULL) ? -1 : pthread_mutex_unlock(m);
}

static inline int sm_mutex_destroy(sm_mutex_t *m)
{
    return (m == NULL) ? -1 : pthread_mutex_destroy(m);
}

static inline int sm_rwlock_init(sm_rwlock_t *m)
{
    if (m == NULL) { return -1; }
    return pthread_rwlock_init(m, NULL);
}

static inline int sm_rwlock_rdlock(sm_rwlock_t *m)
{
    return (m == NULL) ? -1 : pthread_rwlock_rdlock(m);
}

static inline int sm_rwlock_wrlock(sm_rwlock_t *m)
{
    return (m == NULL) ? -1 : pthread_rwlock_wrlock(m);
}

static inline int sm_rwlock_unlock(sm_rwlock_t *m)
{
    return (m == NULL) ? -1 : pthread_rwlock_unlock(m);
}

static inline int sm_rwlock_destroy(sm_rwlock_t *m)
{
    return (m == NULL) ? -1 : pthread_rwlock_destroy(m);
}
#else
typedef struct {
    int dummy; /* unused; keeps the type non-empty for strict compilers */
} sm_mutex_t;

typedef struct {
    int dummy; /* unused; keeps the type non-empty for strict compilers */
} sm_rwlock_t;

#define SM_MUTEX_INITIALIZER { 0 }

static inline int sm_mutex_init(sm_mutex_t *m)
{
    if (m == NULL) { return -1; }
    m->dummy = 0;
    return 0;
}

static inline int sm_mutex_lock(sm_mutex_t *m)
{
    SM_UNUSED(m);
    return 0;
}

static inline int sm_mutex_unlock(sm_mutex_t *m)
{
    SM_UNUSED(m);
    return 0;
}

static inline int sm_mutex_destroy(sm_mutex_t *m)
{
    SM_UNUSED(m);
    return 0;
}

static inline int sm_rwlock_init(sm_rwlock_t *m)
{
    if (m == NULL) { return -1; }
    m->dummy = 0;
    return 0;
}

static inline int sm_rwlock_rdlock(sm_rwlock_t *m)
{
    SM_UNUSED(m);
    return 0;
}

static inline int sm_rwlock_wrlock(sm_rwlock_t *m)
{
    SM_UNUSED(m);
    return 0;
}

static inline int sm_rwlock_unlock(sm_rwlock_t *m)
{
    SM_UNUSED(m);
    return 0;
}

static inline int sm_rwlock_destroy(sm_rwlock_t *m)
{
    SM_UNUSED(m);
    return 0;
}
#endif

#ifdef __cplusplus
}
#endif
#endif /* SM_COMMON_H */
