/**
 * @file subscriber.c
 * @brief Subscriber record + thread-safe SubscriberDB implementation.
 */
#include "subscriber.h"
#include "logger.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <errno.h>

#define MODULE_NAME "SUBSCRIBER"

/* ================= validation ================= */

bool sub_validate_imsi(const char *imsi)
{
    if (imsi == NULL) {
        return false;
    }

    size_t len = strlen(imsi);
    if (len < 6U || len > SM_MAX_IMSI_LEN) {
        return false;
    }

    for (size_t i = 0U; i < len; ++i) {
        if (!isdigit((unsigned char)imsi[i])) {
            return false;
        }
    }
    return true;
}

bool sub_validate_msisdn(const char *msisdn)
{
    if (msisdn == NULL) {
        return false;
    }

    size_t len = strlen(msisdn);
    if (len < 7U || len > SM_MAX_MSISDN_LEN) {
        return false;
    }

    for (size_t i = 0U; i < len; ++i) {
        if (!isdigit((unsigned char)msisdn[i])) {
            return false;
        }
    }
    return true;
}

bool sub_validate_name(const char *name)
{
    if (name == NULL) {
        return false;
    }

    size_t len = strlen(name);
    if (len == 0U || len > SM_MAX_NAME_LEN) {
        return false;
    }

    for (size_t i = 0U; i < len; ++i) {
        unsigned char c = (unsigned char)name[i];
        /* Reject control characters and the CSV delimiter to keep persistence
         * simple and to block basic injection into log/report output. */
        if (iscntrl(c)) {
            return false;
        }
        if (c == ',') {
            return false;
        }
    }
    return true;
}

const char *sub_status_to_str(sm_sub_status_t s)
{
    switch (s) {
        case SUB_STATUS_ACTIVE:     return "ACTIVE";
        case SUB_STATUS_SUSPENDED:  return "SUSPENDED";
        case SUB_STATUS_TERMINATED: return "TERMINATED";
        default:                    return "UNKNOWN";
    }
}

sm_sub_status_t sub_status_from_str(const char *s)
{
    if (s == NULL) return SUB_STATUS_UNKNOWN;
    if (strcmp(s, "ACTIVE") == 0) return SUB_STATUS_ACTIVE;
    if (strcmp(s, "SUSPENDED") == 0) return SUB_STATUS_SUSPENDED;
    if (strcmp(s, "TERMINATED") == 0) return SUB_STATUS_TERMINATED;
    return SUB_STATUS_UNKNOWN;
}

/* ================= lifecycle ================= */

sm_status_t subdb_create(sm_subscriber_db_t **out_db)
{
    if (out_db == NULL) {
        return SM_ERR_NULL_PARAM;
    }

    sm_subscriber_db_t *db = (sm_subscriber_db_t *)calloc(1, sizeof(sm_subscriber_db_t));
    if (db == NULL) {
        return SM_ERR_ALLOC_FAILED;
    }

    sm_status_t st = ht_create(1024U, sm_hash_u64, sm_u64eq, NULL, NULL, &db->by_id);
    if (st != SM_OK) {
        free(db);
        return st;
    }

    st = ht_create(1024U, sm_hash_str, sm_streq, NULL, NULL, &db->by_imsi);
    if (st != SM_OK) {
        ht_destroy(db->by_id);
        free(db);
        return st;
    }

    st = ht_create(1024U, sm_hash_str, sm_streq, NULL, NULL, &db->by_msisdn);
    if (st != SM_OK) {
        ht_destroy(db->by_id);
        ht_destroy(db->by_imsi);
        free(db);
        return st;
    }

    st = mempool_create(sizeof(sm_subscriber_t), 256U, &db->pool);
    if (st != SM_OK) {
        ht_destroy(db->by_id);
        ht_destroy(db->by_imsi);
        ht_destroy(db->by_msisdn);
        free(db);
        return st;
    }

    if (sm_rwlock_init(&db->lock) != 0) {
        mempool_destroy(db->pool);
        ht_destroy(db->by_id);
        ht_destroy(db->by_imsi);
        ht_destroy(db->by_msisdn);
        free(db);
        return SM_ERR_INTERNAL;
    }

    db->next_id = 1U;
    *out_db = db;
    SM_LOG_INFO(MODULE_NAME, "subscriber database created");
    return SM_OK;
}

/* Release a subscriber's shared strings and return its block to the pool.
 * Caller must hold db->lock (write) and must have already removed the
 * subscriber from all three index tables. */
static void free_subscriber_locked(sm_subscriber_db_t *db, sm_subscriber_t *s)
{
    sstr_release(s->name);
    sstr_release(s->region);
    sstr_release(s->plan);
    mempool_free(db->pool, s);
}

void subdb_destroy(sm_subscriber_db_t *db)
{
    if (db == NULL) {
        return;
    }
    (void)sm_rwlock_wrlock(&db->lock);
    /* by_id owns the canonical set of live subscriber pointers. */
    size_t n = 0;
    sm_subscriber_t **all = NULL;
    /* Reuse list_all's collection logic inline to avoid re-locking (we
     * already hold the write lock here, and list_all takes its own lock). */
    {
        extern bool subdb_collect_cb(void *key, void *value, void *user_data);
        typedef struct { sm_subscriber_t **arr; size_t cap; size_t n; } collect_ctx_t;
        collect_ctx_t ctx = { NULL, 0, 0 };
        size_t cap = ht_size(db->by_id);
        if (cap > 0U) {
            ctx.arr = (sm_subscriber_t **)malloc(sizeof(sm_subscriber_t *) * cap);
            ctx.cap = cap;
        }
        ht_iterate(db->by_id, subdb_collect_cb, &ctx);
        all = ctx.arr;
        n = ctx.n;
    }
    for (size_t i = 0; i < n; ++i) {
        free_subscriber_locked(db, all[i]);
    }
    free(all);

    (void)sm_rwlock_unlock(&db->lock);
    ht_destroy(db->by_id);
    ht_destroy(db->by_imsi);
    ht_destroy(db->by_msisdn);
    mempool_destroy(db->pool);
    sm_rwlock_destroy(&db->lock);
    free(db);
    SM_LOG_INFO(MODULE_NAME, "subscriber database destroyed");
}

/* Generic collector used by destroy/list_all/search_by_*: appends `value`
 * (a live sm_subscriber_t*) into a pre-sized array in user_data. */
typedef struct {
    sm_subscriber_t **arr;
    size_t             cap;
    size_t             n;
} collect_ctx_t;

bool subdb_collect_cb(void *key, void *value, void *user_data)
{
    SM_UNUSED(key);
    collect_ctx_t *ctx = (collect_ctx_t *)user_data;
    if (ctx->arr != NULL) {
        if (ctx->n < ctx->cap) {
            ctx->arr[ctx->n] = (sm_subscriber_t *)value;
            ctx->n += 1U;
        }
    }
    return true;
}

/* ================= snapshot helpers ================= */

static sm_subscriber_t *make_snapshot(const sm_subscriber_t *src)
{
    sm_subscriber_t *copy = (sm_subscriber_t *)malloc(sizeof(sm_subscriber_t));
    if (copy == NULL) {
        return NULL;
    }
    *copy = *src;
    /* Take our own references so the snapshot remains valid (and the
     * strings remain alive) even after the DB lock is released and even
     * if the live record is later updated/deleted. */
    copy->name   = sstr_retain(src->name);
    copy->region = sstr_retain(src->region);
    copy->plan   = sstr_retain(src->plan);
    return copy;
}

void subdb_free_one(sm_subscriber_t *s)
{
    if (s == NULL) {
        return;
    }
    sstr_release(s->name);
    sstr_release(s->region);
    sstr_release(s->plan);
    free(s);
}

void subdb_free_results(sm_subscriber_t **array, size_t count)
{
    if (array == NULL) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        subdb_free_one(array[i]);
    }
    free(array);
}

/* ================= CRUD ================= */

sm_status_t subdb_add(sm_subscriber_db_t *db, const char *imsi, const char *msisdn,
                      const char *name, const char *region, const char *plan,
                      sm_sub_status_t status, uint64_t *out_id)
{
    if (db == NULL || imsi == NULL || msisdn == NULL || name == NULL ||
        region == NULL || plan == NULL || out_id == NULL) {
        return SM_ERR_NULL_PARAM;
    }
    if (!sub_validate_imsi(imsi))    { SM_LOG_WARN(MODULE_NAME, "reject: invalid IMSI");    return SM_ERR_INVALID_ARG; }
    if (!sub_validate_msisdn(msisdn)){ SM_LOG_WARN(MODULE_NAME, "reject: invalid MSISDN");  return SM_ERR_INVALID_ARG; }
    if (!sub_validate_name(name))    { SM_LOG_WARN(MODULE_NAME, "reject: invalid name");    return SM_ERR_INVALID_ARG; }
    if (!sub_validate_name(region))  { SM_LOG_WARN(MODULE_NAME, "reject: invalid region");  return SM_ERR_INVALID_ARG; }
    if (!sub_validate_name(plan))    { SM_LOG_WARN(MODULE_NAME, "reject: invalid plan");    return SM_ERR_INVALID_ARG; }

    if (sm_rwlock_wrlock(&db->lock) != 0) {
        return SM_ERR_LOCK_FAILED;
    }

    void *existing = NULL;
    if (ht_get(db->by_imsi, imsi, &existing) == SM_OK) {
        (void)sm_rwlock_unlock(&db->lock);
        SM_LOG_WARN(MODULE_NAME, "duplicate IMSI rejected");
        return SM_ERR_DUPLICATE;
    }
    if (ht_get(db->by_msisdn, msisdn, &existing) == SM_OK) {
        (void)sm_rwlock_unlock(&db->lock);
        SM_LOG_WARN(MODULE_NAME, "duplicate MSISDN rejected");
        return SM_ERR_DUPLICATE;
    }

    sm_subscriber_t *s = (sm_subscriber_t *)mempool_alloc(db->pool);
    if (s == NULL) {
        (void)sm_rwlock_unlock(&db->lock);
        SM_LOG_ERROR(MODULE_NAME, "allocation failed adding subscriber (pool exhausted)");
        return SM_ERR_ALLOC_FAILED;
    }

    (void)strncpy(s->imsi, imsi, SM_MAX_IMSI_LEN);
    s->imsi[SM_MAX_IMSI_LEN] = '\0';
    (void)strncpy(s->msisdn, msisdn, SM_MAX_MSISDN_LEN);
    s->msisdn[SM_MAX_MSISDN_LEN] = '\0';

    sm_status_t st = sstr_intern(name, &s->name);
    if (st == SM_OK) st = sstr_intern(region, &s->region);
    if (st == SM_OK) st = sstr_intern(plan, &s->plan);
    if (st != SM_OK) {
        sstr_release(s->name); sstr_release(s->region); sstr_release(s->plan);
        mempool_free(db->pool, s);
        (void)sm_rwlock_unlock(&db->lock);
        SM_LOG_ERROR(MODULE_NAME, "string interning failed while adding subscriber: %s",
                     sm_status_str(st));
        return st;
    }

    s->status        = status;
    s->subscriber_id = db->next_id++;
    s->created_at    = time(NULL);
    s->updated_at    = s->created_at;

    /* Insert into all three indexes. If any insert fails, roll back the
     * ones that succeeded so the DB never ends up partially indexed. */
    st = ht_put(db->by_id, &s->subscriber_id, s, false);
    if (st == SM_OK) st = ht_put(db->by_imsi, s->imsi, s, false);
    if (st == SM_OK) st = ht_put(db->by_msisdn, s->msisdn, s, false);

    if (st != SM_OK) {
        SM_LOG_ERROR(MODULE_NAME, "index insert failed, rolling back: %s", sm_status_str(st));
        (void)ht_remove(db->by_id, &s->subscriber_id);
        (void)ht_remove(db->by_imsi, s->imsi);
        (void)ht_remove(db->by_msisdn, s->msisdn);
        free_subscriber_locked(db, s);
        (void)sm_rwlock_unlock(&db->lock);
        return st;
    }

    *out_id = s->subscriber_id;
    (void)sm_rwlock_unlock(&db->lock);
    SM_LOG_INFO(MODULE_NAME, "subscriber added id=%llu", (unsigned long long)*out_id);
    return SM_OK;
}

sm_status_t subdb_delete(sm_subscriber_db_t *db, uint64_t id)
{
    if (db == NULL) {
        return SM_ERR_NULL_PARAM;
    }
    if (sm_rwlock_wrlock(&db->lock) != 0) {
        return SM_ERR_LOCK_FAILED;
    }
    void *found = NULL;
    if (ht_get(db->by_id, &id, &found) != SM_OK) {
        (void)sm_rwlock_unlock(&db->lock);
        return SM_ERR_NOT_FOUND;
    }
    sm_subscriber_t *s = (sm_subscriber_t *)found;
    (void)ht_remove(db->by_id, &s->subscriber_id);
    (void)ht_remove(db->by_imsi, s->imsi);
    (void)ht_remove(db->by_msisdn, s->msisdn);
    free_subscriber_locked(db, s);
    (void)sm_rwlock_unlock(&db->lock);
    SM_LOG_INFO(MODULE_NAME, "subscriber deleted id=%llu", (unsigned long long)id);
    return SM_OK;
}

sm_status_t subdb_update(sm_subscriber_db_t *db, uint64_t id,
                         const char *name, const char *region,
                         const char *plan, const sm_sub_status_t *status)
{
    if (db == NULL) {
        return SM_ERR_NULL_PARAM;
    }
    if ((name != NULL && !sub_validate_name(name)) ||
        (region != NULL && !sub_validate_name(region)) ||
        (plan != NULL && !sub_validate_name(plan))) {
        return SM_ERR_INVALID_ARG;
    }

    if (sm_rwlock_wrlock(&db->lock) != 0) {
        return SM_ERR_LOCK_FAILED;
    }
    void *found = NULL;
    if (ht_get(db->by_id, &id, &found) != SM_OK) {
        (void)sm_rwlock_unlock(&db->lock);
        return SM_ERR_NOT_FOUND;
    }
    sm_subscriber_t *s = (sm_subscriber_t *)found;

    if (name != NULL) {
        sm_shared_string_t *ns = NULL;
        if (sstr_intern(name, &ns) == SM_OK) {
            sstr_release(s->name);
            s->name = ns;
        }
    }
    if (region != NULL) {
        sm_shared_string_t *ns = NULL;
        if (sstr_intern(region, &ns) == SM_OK) {
            sstr_release(s->region);
            s->region = ns;
        }
    }
    if (plan != NULL) {
        sm_shared_string_t *ns = NULL;
        if (sstr_intern(plan, &ns) == SM_OK) {
            sstr_release(s->plan);
            s->plan = ns;
        }
    }
    if (status != NULL) {
        s->status = *status;
    }
    s->updated_at = time(NULL);

    (void)sm_rwlock_unlock(&db->lock);
    SM_LOG_INFO(MODULE_NAME, "subscriber updated id=%llu", (unsigned long long)id);
    return SM_OK;
}

size_t subdb_count(sm_subscriber_db_t *db)
{
    return (db == NULL) ? 0U : ht_size(db->by_id);
}

/* ================= search ================= */

sm_status_t subdb_search_by_id(sm_subscriber_db_t *db, uint64_t id, sm_subscriber_t **out)
{
    if (db == NULL || out == NULL) {
        return SM_ERR_NULL_PARAM;
    }
    if (sm_rwlock_rdlock(&db->lock) != 0) {
        return SM_ERR_LOCK_FAILED;
    }
    void *found = NULL;
    sm_status_t st = ht_get(db->by_id, &id, &found);
    if (st == SM_OK) {
        *out = make_snapshot((sm_subscriber_t *)found);
        if (*out == NULL) st = SM_ERR_ALLOC_FAILED;
    }
    (void)sm_rwlock_unlock(&db->lock);
    return st;
}

sm_status_t subdb_search_by_imsi(sm_subscriber_db_t *db, const char *imsi, sm_subscriber_t **out)
{
    if (db == NULL || imsi == NULL || out == NULL) {
        return SM_ERR_NULL_PARAM;
    }
    if (sm_rwlock_rdlock(&db->lock) != 0) {
        return SM_ERR_LOCK_FAILED;
    }
    void *found = NULL;
    sm_status_t st = ht_get(db->by_imsi, imsi, &found);
    if (st == SM_OK) {
        *out = make_snapshot((sm_subscriber_t *)found);
        if (*out == NULL) st = SM_ERR_ALLOC_FAILED;
    }
    (void)sm_rwlock_unlock(&db->lock);
    return st;
}

sm_status_t subdb_search_by_msisdn(sm_subscriber_db_t *db, const char *msisdn, sm_subscriber_t **out)
{
    if (db == NULL || msisdn == NULL || out == NULL) {
        return SM_ERR_NULL_PARAM;
    }
    if (sm_rwlock_rdlock(&db->lock) != 0) {
        return SM_ERR_LOCK_FAILED;
    }
    void *found = NULL;
    sm_status_t st = ht_get(db->by_msisdn, msisdn, &found);
    if (st == SM_OK) {
        *out = make_snapshot((sm_subscriber_t *)found);
        if (*out == NULL) st = SM_ERR_ALLOC_FAILED;
    }
    (void)sm_rwlock_unlock(&db->lock);
    return st;
}

/* Context + callback for the three filtered-search functions below. Each
 * filter predicate runs while the read lock is held, then matches are
 * snapshotted into a dynamically-grown array. */
typedef struct {
    sm_subscriber_t **arr;
    size_t             cap;
    size_t             n;
    bool               ok;
} filter_ctx_t;

static bool filter_grow(filter_ctx_t *ctx)
{
    size_t new_cap = (ctx->cap == 0U) ? 16U : (ctx->cap * 2U);
    sm_subscriber_t **new_arr = (sm_subscriber_t **)realloc(ctx->arr, new_cap * sizeof(sm_subscriber_t *));
    if (new_arr == NULL) {
        return false;
    }
    ctx->arr = new_arr;
    ctx->cap = new_cap;
    return true;
}

/* We need the filter value captured too, so wrap it alongside the ctx. */
typedef struct {
    filter_ctx_t ctx;
    char         needle[SM_MAX_NAME_LEN + 1U];
    sm_sub_status_t status_needle;
} named_filter_ctx_t;

static bool region_match_cb(void *key, void *value, void *user_data)
{
    SM_UNUSED(key);
    named_filter_ctx_t *nctx = (named_filter_ctx_t *)user_data;
    const sm_subscriber_t *s = (const sm_subscriber_t *)value;
    if (strcmp(sstr_cstr(s->region), nctx->needle) == 0) {
        if (nctx->ctx.n >= nctx->ctx.cap) {
            if (!filter_grow(&nctx->ctx)) {
                nctx->ctx.ok = false;
                return false;
            }
        }
        sm_subscriber_t *copy = make_snapshot(s);
        if (copy == NULL) {
            nctx->ctx.ok = false;
            return false;
        }
        nctx->ctx.arr[nctx->ctx.n] = copy;
        nctx->ctx.n += 1U;
    }
    return true;
}

static bool plan_match_cb(void *key, void *value, void *user_data)
{
    SM_UNUSED(key);
    named_filter_ctx_t *nctx = (named_filter_ctx_t *)user_data;
    const sm_subscriber_t *s = (const sm_subscriber_t *)value;
    if (strcmp(sstr_cstr(s->plan), nctx->needle) == 0) {
        if (nctx->ctx.n >= nctx->ctx.cap) {
            if (!filter_grow(&nctx->ctx)) {
                nctx->ctx.ok = false;
                return false;
            }
        }
        sm_subscriber_t *copy = make_snapshot(s);
        if (copy == NULL) {
            nctx->ctx.ok = false;
            return false;
        }
        nctx->ctx.arr[nctx->ctx.n] = copy;
        nctx->ctx.n += 1U;
    }
    return true;
}

static bool status_match_cb(void *key, void *value, void *user_data)
{
    SM_UNUSED(key);
    named_filter_ctx_t *nctx = (named_filter_ctx_t *)user_data;
    const sm_subscriber_t *s = (const sm_subscriber_t *)value;
    if (s->status == nctx->status_needle) {
        if (nctx->ctx.n >= nctx->ctx.cap) {
            if (!filter_grow(&nctx->ctx)) {
                nctx->ctx.ok = false;
                return false;
            }
        }
        sm_subscriber_t *copy = make_snapshot(s);
        if (copy == NULL) {
            nctx->ctx.ok = false;
            return false;
        }
        nctx->ctx.arr[nctx->ctx.n] = copy;
        nctx->ctx.n += 1U;
    }
    return true;
}

static bool listall_cb(void *key, void *value, void *user_data)
{
    SM_UNUSED(key);
    named_filter_ctx_t *nctx = (named_filter_ctx_t *)user_data;
    const sm_subscriber_t *s = (const sm_subscriber_t *)value;
    if (nctx->ctx.n >= nctx->ctx.cap) {
        if (!filter_grow(&nctx->ctx)) {
            nctx->ctx.ok = false;
            return false;
        }
    }
    sm_subscriber_t *copy = make_snapshot(s);
    if (copy == NULL) {
        nctx->ctx.ok = false;
        return false;
    }
    nctx->ctx.arr[nctx->ctx.n] = copy;
    nctx->ctx.n += 1U;
    return true;
}

sm_status_t subdb_search_by_region(sm_subscriber_db_t *db, const char *region,
                                   sm_subscriber_t ***out_array, size_t *out_count)
{
    if (db == NULL || region == NULL || out_array == NULL || out_count == NULL) {
        return SM_ERR_NULL_PARAM;
    }
    named_filter_ctx_t nctx = {0};
    nctx.ctx.ok = true;
    (void)strncpy(nctx.needle, region, SM_MAX_NAME_LEN);
    nctx.needle[SM_MAX_NAME_LEN] = '\0';

    (void)sm_rwlock_rdlock(&db->lock);
    ht_iterate(db->by_id, region_match_cb, &nctx);
    (void)sm_rwlock_unlock(&db->lock);

    if (!nctx.ctx.ok) {
        subdb_free_results(nctx.ctx.arr, nctx.ctx.n);
        return SM_ERR_ALLOC_FAILED;
    }
    *out_array = nctx.ctx.arr;
    *out_count = nctx.ctx.n;
    return SM_OK;
}

sm_status_t subdb_search_by_plan(sm_subscriber_db_t *db, const char *plan,
                                 sm_subscriber_t ***out_array, size_t *out_count)
{
    if (db == NULL || plan == NULL || out_array == NULL || out_count == NULL) {
        return SM_ERR_NULL_PARAM;
    }
    named_filter_ctx_t nctx = {0};
    nctx.ctx.ok = true;
    (void)strncpy(nctx.needle, plan, SM_MAX_NAME_LEN);
    nctx.needle[SM_MAX_NAME_LEN] = '\0';

    (void)sm_rwlock_rdlock(&db->lock);
    ht_iterate(db->by_id, plan_match_cb, &nctx);
    (void)sm_rwlock_unlock(&db->lock);

    if (!nctx.ctx.ok) {
        subdb_free_results(nctx.ctx.arr, nctx.ctx.n);
        return SM_ERR_ALLOC_FAILED;
    }
    *out_array = nctx.ctx.arr;
    *out_count = nctx.ctx.n;
    return SM_OK;
}

sm_status_t subdb_search_by_status(sm_subscriber_db_t *db, sm_sub_status_t status,
                                   sm_subscriber_t ***out_array, size_t *out_count)
{
    if (db == NULL || out_array == NULL || out_count == NULL) {
        return SM_ERR_NULL_PARAM;
    }
    named_filter_ctx_t nctx = {0};
    nctx.ctx.ok = true;
    nctx.status_needle = status;

    (void)sm_rwlock_rdlock(&db->lock);
    ht_iterate(db->by_id, status_match_cb, &nctx);
    (void)sm_rwlock_unlock(&db->lock);

    if (!nctx.ctx.ok) {
        subdb_free_results(nctx.ctx.arr, nctx.ctx.n);
        return SM_ERR_ALLOC_FAILED;
    }
    *out_array = nctx.ctx.arr;
    *out_count = nctx.ctx.n;
    return SM_OK;
}

sm_status_t subdb_list_all(sm_subscriber_db_t *db, sm_subscriber_t ***out_array, size_t *out_count)
{
    if (db == NULL || out_array == NULL || out_count == NULL) {
        return SM_ERR_NULL_PARAM;
    }
    named_filter_ctx_t nctx = {0};
    nctx.ctx.ok = true;

    (void)sm_rwlock_rdlock(&db->lock);
    ht_iterate(db->by_id, listall_cb, &nctx);
    (void)sm_rwlock_unlock(&db->lock);

    if (!nctx.ctx.ok) {
        subdb_free_results(nctx.ctx.arr, nctx.ctx.n);
        return SM_ERR_ALLOC_FAILED;
    }
    *out_array = nctx.ctx.arr;
    *out_count = nctx.ctx.n;
    return SM_OK;
}

/* ================= sorting ================= */

static int compare_subs(const sm_subscriber_t *a, const sm_subscriber_t *b, sm_sort_key_t key)
{
    switch (key) {
        case SORT_BY_ID:
            if (a->subscriber_id < b->subscriber_id) return -1;
            if (a->subscriber_id > b->subscriber_id) return 1;
            return 0;
        case SORT_BY_REGION:
            return strcmp(sstr_cstr(a->region), sstr_cstr(b->region));
        case SORT_BY_PLAN:
            return strcmp(sstr_cstr(a->plan), sstr_cstr(b->plan));
        case SORT_BY_STATUS:
            return strcmp(sub_status_to_str(a->status), sub_status_to_str(b->status));
        default:
            return 0;
    }
}

/* ---- merge sort: O(n log n) worst case, stable ---- */
static void merge_sort_rec(sm_subscriber_t **arr, sm_subscriber_t **tmp,
                           size_t lo, size_t hi, sm_sort_key_t key)
{
    if (hi - lo <= 1U) {
        return;
    }
    size_t mid = lo + (hi - lo) / 2U;
    merge_sort_rec(arr, tmp, lo, mid, key);
    merge_sort_rec(arr, tmp, mid, hi, key);

    size_t i = lo, j = mid, k = lo;
    while (i < mid && j < hi) {
        if (compare_subs(arr[i], arr[j], key) <= 0) {
            tmp[k++] = arr[i++];
        } else {
            tmp[k++] = arr[j++];
        }
    }
    while (i < mid) tmp[k++] = arr[i++];
    while (j < hi)  tmp[k++] = arr[j++];
    for (size_t x = lo; x < hi; ++x) arr[x] = tmp[x];
}

void sub_sort_merge(sm_subscriber_t **array, size_t count, sm_sort_key_t key)
{
    if (array == NULL || count < 2U) {
        return;
    }
    sm_subscriber_t **tmp = (sm_subscriber_t **)malloc(count * sizeof(sm_subscriber_t *));
    if (tmp == NULL) {
        SM_LOG_ERROR(MODULE_NAME, "merge sort: temp buffer allocation failed, array left unsorted");
        return;
    }
    merge_sort_rec(array, tmp, 0U, count, key);
    free(tmp);
}

/* ---- quicksort: average O(n log n), in-place, median-of-three pivot ---- */
static void swap_ptr(sm_subscriber_t **a, sm_subscriber_t **b)
{
    sm_subscriber_t *t = *a; *a = *b; *b = t;
}

static size_t median_of_three(sm_subscriber_t **arr, size_t lo, size_t mid, size_t hi, sm_sort_key_t key)
{
    if (compare_subs(arr[lo], arr[mid], key) > 0) swap_ptr(&arr[lo], &arr[mid]);
    if (compare_subs(arr[lo], arr[hi], key) > 0)  swap_ptr(&arr[lo], &arr[hi]);
    if (compare_subs(arr[mid], arr[hi], key) > 0) swap_ptr(&arr[mid], &arr[hi]);
    return mid;
}

static void quick_sort_rec(sm_subscriber_t **arr, long lo, long hi, sm_sort_key_t key)
{
    while (lo < hi) {
        if (hi - lo < 12) {
            /* Insertion sort for small partitions: fewer comparisons than
             * recursing all the way down, and better cache behavior. */
            for (long i = lo + 1; i <= hi; ++i) {
                sm_subscriber_t *key_val = arr[i];
                long j = i - 1;
                while (j >= lo && compare_subs(arr[j], key_val, key) > 0) {
                    arr[j + 1] = arr[j];
                    j--;
                }
                arr[j + 1] = key_val;
            }
            return;
        }
        size_t mid = median_of_three(arr, (size_t)lo, (size_t)(lo + (hi - lo) / 2), (size_t)hi, key);
        swap_ptr(&arr[mid], &arr[hi]);
        sm_subscriber_t *const pivot = arr[hi];

        long i = lo - 1;
        for (long j = lo; j < hi; ++j) {
            if (compare_subs(arr[j], pivot, key) <= 0) {
                i++;
                swap_ptr(&arr[i], &arr[j]);
            }
        }
        swap_ptr(&arr[i + 1], &arr[hi]);
        long p = i + 1;

        /* Recurse into the smaller partition, loop over the larger one:
         * bounds worst-case stack depth to O(log n). */
        if (p - lo < hi - p) {
            quick_sort_rec(arr, lo, p - 1, key);
            lo = p + 1;
        } else {
            quick_sort_rec(arr, p + 1, hi, key);
            hi = p - 1;
        }
    }
}

void sub_sort_quick(sm_subscriber_t **array, size_t count, sm_sort_key_t key)
{
    if (array == NULL || count < 2U) {
        return;
    }
    quick_sort_rec(array, 0L, (long)count - 1L, key);
}

/* ================= persistence (CSV) ================= */

#define CSV_HEADER "subscriber_id,imsi,msisdn,name,region,plan,status,created_at,updated_at\n"

sm_status_t subdb_save_csv(sm_subscriber_db_t *db, const char *path)
{
    if (db == NULL || path == NULL) {
        return SM_ERR_NULL_PARAM;
    }
    char tmp_path[600];
    (void)snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    FILE *fp = fopen(tmp_path, "w");
    if (fp == NULL) {
        SM_LOG_ERROR(MODULE_NAME, "save_csv: cannot open temp file '%s': %s", tmp_path, strerror(errno));
        return SM_ERR_IO;
    }
    if (fputs(CSV_HEADER, fp) < 0) {
        fclose(fp);
        return SM_ERR_IO;
    }

    sm_subscriber_t **all = NULL;
    size_t n = 0;
    sm_status_t st = subdb_list_all(db, &all, &n);
    if (st != SM_OK) {
        fclose(fp);
        (void)remove(tmp_path);
        return st;
    }

    bool write_ok = true;
    for (size_t i = 0; i < n && write_ok; ++i) {
        const sm_subscriber_t *s = all[i];
        int rc = fprintf(fp, "%llu,%s,%s,%s,%s,%s,%s,%ld,%ld\n",
                          (unsigned long long)s->subscriber_id, s->imsi, s->msisdn,
                          sstr_cstr(s->name), sstr_cstr(s->region), sstr_cstr(s->plan),
                          sub_status_to_str(s->status),
                          (long)s->created_at, (long)s->updated_at);
        if (rc < 0) write_ok = false;
    }
    subdb_free_results(all, n);

    /* fflush() is the durability step available in standard C; fsync()
     * (forcing the OS to flush to physical disk) is POSIX-only and has
     * been dropped so this file builds with plain ISO C11. */
    if (fflush(fp) != 0) write_ok = false;
    fclose(fp);

    if (!write_ok) {
        (void)remove(tmp_path);
        SM_LOG_ERROR(MODULE_NAME, "save_csv: write failure, aborting save");
        return SM_ERR_IO;
    }

    /* Atomic on POSIX: rename() is a single filesystem operation, so readers
     * never observe a partially-written file at `path`. On platforms such as
     * Windows, an existing destination must be removed first because rename()
     * does not overwrite it. */
    if (remove(path) != 0 && errno != ENOENT) {
        SM_LOG_ERROR(MODULE_NAME, "save_csv: could not replace existing file '%s': %s", path, strerror(errno));
        (void)remove(tmp_path);
        return SM_ERR_IO;
    }
    if (rename(tmp_path, path) != 0) {
        SM_LOG_ERROR(MODULE_NAME, "save_csv: rename failed: %s", strerror(errno));
        (void)remove(tmp_path);
        return SM_ERR_IO;
    }
    SM_LOG_INFO(MODULE_NAME, "saved %llu subscriber(s) to '%s'", (unsigned long long)n, path);
    return SM_OK;
}

sm_status_t subdb_load_csv(sm_subscriber_db_t *db, const char *path)
{
    if (db == NULL || path == NULL) {
        return SM_ERR_NULL_PARAM;
    }
    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        SM_LOG_ERROR(MODULE_NAME, "load_csv: cannot open '%s': %s", path, strerror(errno));
        return SM_ERR_IO;
    }

    char line[SM_MAX_LINE_LEN];
    if (fgets(line, sizeof(line), fp) == NULL) {
        fclose(fp);
        return SM_ERR_CORRUPT_DATA; /* missing header */
    }
    if (strncmp(line, "subscriber_id,", 14) != 0) {
        fclose(fp);
        SM_LOG_ERROR(MODULE_NAME, "load_csv: unrecognized header, refusing to load");
        return SM_ERR_CORRUPT_DATA;
    }

    size_t loaded = 0, skipped = 0, line_no = 1;
    while (fgets(line, sizeof(line), fp) != NULL) {
        line_no++;
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
        if (line[0] == '\0') continue;

        char id_s[32], imsi[SM_MAX_IMSI_LEN + 1], msisdn[SM_MAX_MSISDN_LEN + 1];
        char name[SM_MAX_NAME_LEN + 1], region[SM_MAX_NAME_LEN + 1], plan[SM_MAX_NAME_LEN + 1];
        char status_s[16];
        long created = 0, updated = 0;

        int fields = sscanf(line, "%31[^,],%15[^,],%15[^,],%64[^,],%64[^,],%64[^,],%15[^,],%ld,%ld",
                             id_s, imsi, msisdn, name, region, plan, status_s, &created, &updated);
        if (fields != 9) {
            SM_LOG_WARN(MODULE_NAME, "load_csv: malformed line %llu, skipping",
                        (unsigned long long)line_no);
            skipped++;
            continue;
        }
        if (!sub_validate_imsi(imsi) || !sub_validate_msisdn(msisdn) || !sub_validate_name(name) ||
            !sub_validate_name(region) || !sub_validate_name(plan)) {
            SM_LOG_WARN(MODULE_NAME, "load_csv: invalid fields on line %llu, skipping",
                        (unsigned long long)line_no);
            skipped++;
            continue;
        }

        uint64_t out_id = 0;
        sm_sub_status_t status = sub_status_from_str(status_s);
        sm_status_t ast = subdb_add(db, imsi, msisdn, name, region, plan, status, &out_id);
        if (ast != SM_OK) {
            SM_LOG_WARN(MODULE_NAME, "load_csv: could not add line %llu (%s), skipping",
                        (unsigned long long)line_no, sm_status_str(ast));
            skipped++;
            continue;
        }
        loaded++;
    }
    fclose(fp);
    SM_LOG_INFO(MODULE_NAME, "load_csv: loaded=%llu skipped=%llu from '%s'",
                (unsigned long long)loaded, (unsigned long long)skipped, path);
    return SM_OK;
}

sm_status_t subdb_backup(sm_subscriber_db_t *db, const char *backup_dir, char *out_path, size_t out_path_len)
{
    if (db == NULL || backup_dir == NULL || out_path == NULL) {
        return SM_ERR_NULL_PARAM;
    }
    /* Portable, standard-C-only build: there is no ISO C directory-creation
     * call, so the backup directory is expected to already exist (the
     * Makefile's "dirs" target creates data/backups on every build). If it
     * does not exist, the fopen() inside subdb_save_csv() below will fail
     * and that failure is reported to the caller as SM_ERR_IO. */
    time_t now = time(NULL);
    const struct tm *tm_info = localtime(&now); /* single-threaded: plain localtime() is fine */
    char stamp[32];
    if (tm_info != NULL) {
        (void)strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", tm_info);
    } else {
        (void)snprintf(stamp, sizeof(stamp), "unknown_time");
    }

    (void)snprintf(out_path, out_path_len, "%s/subscribers_%s.csv", backup_dir, stamp);
    return subdb_save_csv(db, out_path);
}

sm_status_t subdb_restore(sm_subscriber_db_t *db, const char *backup_path)
{
    if (db == NULL || backup_path == NULL) {
        return SM_ERR_NULL_PARAM;
    }
    if (subdb_count(db) != 0U) {
        SM_LOG_WARN(MODULE_NAME, "restore: target DB is not empty; records will be merged, "
                    "duplicates (same IMSI/MSISDN) will be skipped");
    }
    return subdb_load_csv(db, backup_path);
}
