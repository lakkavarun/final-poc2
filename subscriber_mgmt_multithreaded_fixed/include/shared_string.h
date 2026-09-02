/**
 * @file shared_string.h
 * @brief Reference-counted, interned strings.
 *
 * Rationale: fields like region ("DELHI"), plan ("PREPAID_GOLD"), and status
 * ("ACTIVE") repeat across millions of subscriber records. Interning stores
 * each distinct string exactly once and hands out refcounted handles,
 * turning O(millions) string allocations into O(distinct values).
 *
 * Thread safety: the global intern pool and every ->refcount mutation
 * (sstr_intern, sstr_retain, sstr_release) are guarded by one internal
 * mutex (see shared_string.c), so a handle may be interned/retained/
 * released concurrently from any number of threads.
 */
#ifndef SM_SHARED_STRING_H
#define SM_SHARED_STRING_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char             *data;   /* NUL-terminated, immutable after creation */
    size_t            len;
    uint32_t          refcount;
} sm_shared_string_t;

/** Initialize the global intern pool. Call once at startup. */
sm_status_t sstr_pool_init(size_t initial_capacity_hint);

/** Destroy the global intern pool. Asserts (logs) if strings are still live. */
void sstr_pool_shutdown(void);

/**
 * Intern `text`: if an identical string already exists, its refcount is
 * incremented and the existing handle is returned; otherwise a new entry
 * is created with refcount 1. Caller owns one reference and must eventually
 * call sstr_release().
 * @return SM_OK with *out set, or SM_ERR_ALLOC_FAILED / SM_ERR_INVALID_ARG.
 * Time complexity: O(len) to hash + O(1) average lookup.
 */
sm_status_t sstr_intern(const char *text, sm_shared_string_t **out);

/** Take an additional reference on an already-held shared string. */
sm_shared_string_t *sstr_retain(sm_shared_string_t *s);

/** Release one reference; frees the interned string when refcount hits 0. */
void sstr_release(sm_shared_string_t *s);

/** Convenience accessor; returns "" for NULL rather than crashing callers. */
const char *sstr_cstr(const sm_shared_string_t *s);

typedef struct {
    size_t   distinct_strings;
    size_t   total_references;
    size_t   bytes_for_distinct_strings;
    size_t   bytes_would_be_used_without_interning; /* total_references * avg bytes */
} sm_sstr_pool_stats_t;

/** Snapshot pool-wide stats, useful for the "shared string savings" report. */
sm_status_t sstr_pool_get_stats(sm_sstr_pool_stats_t *out);

#ifdef __cplusplus
}
#endif
#endif /* SM_SHARED_STRING_H */
