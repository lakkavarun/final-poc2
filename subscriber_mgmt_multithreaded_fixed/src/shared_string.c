/**
 * @file shared_string.c
 * @brief Implementation of the reference-counted string interning pool.
 */
#include "shared_string.h"
#include "hash_table.h"
#include "logger.h"
#include <stdlib.h>
#include <string.h>

#define MODULE_NAME "SHAREDSTR"

static sm_hash_table_t *g_pool          = NULL; /* char* -> sm_shared_string_t* */
static sm_mutex_t  g_intern_lock   = SM_MUTEX_INITIALIZER;
static bool              g_initialized  = false;

sm_status_t sstr_pool_init(size_t initial_capacity_hint)
{
    if (g_initialized) {
        return SM_OK; /* idempotent */
    }
    sm_status_t st = ht_create(initial_capacity_hint, sm_hash_str, sm_streq,
                                NULL /* keys are owned by the value's ->data */,
                                NULL /* values freed explicitly on release  */,
                                &g_pool);
    if (st != SM_OK) {
        return st;
    }
    g_initialized = true;
    SM_LOG_INFO(MODULE_NAME, "shared string pool initialized (hint=%llu)",
                (unsigned long long)initial_capacity_hint);
    return SM_OK;
}

void sstr_pool_shutdown(void)
{
    if (!g_initialized) {
        return;
    }
    size_t remaining = ht_size(g_pool);
    if (remaining > 0U) {
        SM_LOG_WARN(MODULE_NAME,
                    "shutting down with %llu interned string(s) still referenced "
                    "(likely a missing sstr_release somewhere)",
                    (unsigned long long)remaining);
    }
    /* Free every remaining sm_shared_string_t and its backing buffer. */
    ht_destroy(g_pool); /* free fns are NULL, so this only frees table nodes;
                           leaked strings are logged above intentionally so
                           the leak is visible rather than silently masked. */
    g_pool = NULL;
    g_initialized = false;
}

sm_status_t sstr_intern(const char *text, sm_shared_string_t **out)
{
    if (text == NULL || out == NULL) {
        return SM_ERR_NULL_PARAM;
    }
    if (!g_initialized) {
        return SM_ERR_INTERNAL;
    }
    size_t len = strlen(text);
    if (len == 0U || len > 255U) {
        return SM_ERR_INVALID_ARG;
    }

    if (sm_mutex_lock(&g_intern_lock) != 0) {
        return SM_ERR_LOCK_FAILED;
    }

    void *existing = NULL;
    if (ht_get(g_pool, text, &existing) == SM_OK) {
        sm_shared_string_t *s = (sm_shared_string_t *)existing;
        s->refcount += 1U;
        (void)sm_mutex_unlock(&g_intern_lock);
        *out = s;
        return SM_OK;
    }

    sm_shared_string_t *s = (sm_shared_string_t *)malloc(sizeof(sm_shared_string_t));
    if (s == NULL) {
        (void)sm_mutex_unlock(&g_intern_lock);
        return SM_ERR_ALLOC_FAILED;
    }
    s->data = (char *)malloc(len + 1U);
    if (s->data == NULL) {
        free(s);
        (void)sm_mutex_unlock(&g_intern_lock);
        return SM_ERR_ALLOC_FAILED;
    }
    memcpy(s->data, text, len + 1U);
    s->len = len;
    s->refcount = 1U;

    sm_status_t pst = ht_put(g_pool, s->data, s, false);
    if (pst != SM_OK) {
        free(s->data);
        free(s);
        (void)sm_mutex_unlock(&g_intern_lock);
        return pst;
    }

    (void)sm_mutex_unlock(&g_intern_lock);
    *out = s;
    return SM_OK;
}

sm_shared_string_t *sstr_retain(sm_shared_string_t *s)
{
    if (s == NULL) {
        return NULL;
    }
    /* refcount is shared, mutable state also touched by sstr_intern() and
     * sstr_release() from other threads -- take the same intern-pool lock
     * they use so the increment can never race with those. */
    if (sm_mutex_lock(&g_intern_lock) == 0) {
        s->refcount += 1U;
        (void)sm_mutex_unlock(&g_intern_lock);
    }
    return s;
}

void sstr_release(sm_shared_string_t *s)
{
    if (s == NULL) {
        return;
    }
    if (sm_mutex_lock(&g_intern_lock) != 0) {
        return;
    }
    uint32_t prev = s->refcount;
    s->refcount -= 1U;
    if (prev == 0U) {
        /* Underflow: double-release bug. Log loudly, do not free again. */
        s->refcount += 1U; /* undo */
        (void)sm_mutex_unlock(&g_intern_lock);
        SM_LOG_FATAL(MODULE_NAME, "double-release detected on shared string '%s'",
                     s->data != NULL ? s->data : "?");
        return;
    }
    if (prev == 1U) {
        /* We just took it to 0: remove from the pool and free, all still
         * under the same lock acquisition that did the decrement -- no
         * window where another thread can observe/retain a refcount-0
         * entry that is about to be freed. */
        (void)ht_remove(g_pool, s->data); /* free fns are NULL: table won't double-free */
        (void)sm_mutex_unlock(&g_intern_lock);
        free(s->data);
        free(s);
        return;
    }
    (void)sm_mutex_unlock(&g_intern_lock);
}

const char *sstr_cstr(const sm_shared_string_t *s)
{
    return (s != NULL && s->data != NULL) ? s->data : "";
}

/* Accumulator used while iterating the pool for statistics. */
typedef struct {
    size_t distinct;
    size_t refs;
    size_t bytes;
} sstr_stats_acc_t;

static bool sstr_stats_iter_cb(void *key, void *value, void *user_data)
{
    SM_UNUSED(key);
    sstr_stats_acc_t *acc = (sstr_stats_acc_t *)user_data;
    sm_shared_string_t *s = (sm_shared_string_t *)value;
    acc->distinct += 1U;
    acc->refs     += s->refcount;
    acc->bytes    += s->len + 1U;
    return true;
}

sm_status_t sstr_pool_get_stats(sm_sstr_pool_stats_t *out)
{
    if (out == NULL) {
        return SM_ERR_NULL_PARAM;
    }
    if (!g_initialized) {
        return SM_ERR_INTERNAL;
    }
    memset(out, 0, sizeof(*out));

    sstr_stats_acc_t acc = {0, 0, 0};
    ht_iterate(g_pool, sstr_stats_iter_cb, &acc);

    out->distinct_strings                      = acc.distinct;
    out->total_references                      = acc.refs;
    out->bytes_for_distinct_strings            = acc.bytes;
    out->bytes_would_be_used_without_interning =
        (acc.distinct > 0U) ? (acc.refs * (acc.bytes / acc.distinct)) : 0U;
    return SM_OK;
}
