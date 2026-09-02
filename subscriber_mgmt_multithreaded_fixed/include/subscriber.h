/**
 * @file subscriber.h
 * @brief Subscriber record definition and the thread-safe SubscriberDB that
 *        indexes subscribers by ID, IMSI, and MSISDN.
 *
 * Design notes:
 *  - subscriber_id / IMSI / MSISDN are unique per subscriber, so they are
 *    stored as plain fixed-size buffers (interning unique values only adds
 *    overhead).
 *  - name / region / plan / status are highly repetitive across millions of
 *    subscribers (a handful of plans, regions, and statuses), so they are
 *    stored as sm_shared_string_t* via the interning pool — this is where
 *    the real memory win comes from.
 *
 * Thread safety: SubscriberDB is guarded by a single sm_rwlock_t that
 * covers all three indexes together, because add/delete/update must keep
 * the three indexes consistent with each other (an id->imsi->msisdn triple
 * cannot be allowed to go out of sync under concurrent access).
 */
#ifndef SM_SUBSCRIBER_H
#define SM_SUBSCRIBER_H

#include "common.h"
#include "shared_string.h"
#include "hash_table.h"
#include "memory_pool.h"
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SUB_STATUS_ACTIVE = 0,
    SUB_STATUS_SUSPENDED,
    SUB_STATUS_TERMINATED,
    SUB_STATUS_UNKNOWN
} sm_sub_status_t;

typedef struct {
    uint64_t             subscriber_id;
    char                  imsi[SM_MAX_IMSI_LEN + 1U];
    char                  msisdn[SM_MAX_MSISDN_LEN + 1U];
    sm_shared_string_t   *name;
    sm_shared_string_t   *region;
    sm_shared_string_t   *plan;
    sm_sub_status_t       status;
    time_t                created_at;
    time_t                updated_at;
} sm_subscriber_t;

typedef struct {
    sm_hash_table_t   *by_id;      /* uint64_t*          -> sm_subscriber_t* */
    sm_hash_table_t   *by_imsi;    /* char* (points into subscriber->imsi)   */
    sm_hash_table_t   *by_msisdn;  /* char* (points into subscriber->msisdn) */
    sm_memory_pool_t  *pool;       /* backing store for sm_subscriber_t      */
    sm_rwlock_t   lock;       /* coarse lock covering all 3 indexes     */
    uint64_t            next_id;
} sm_subscriber_db_t;

/* ---------------- lifecycle ---------------- */

sm_status_t subdb_create(sm_subscriber_db_t **out_db);
void subdb_destroy(sm_subscriber_db_t *db);

/* ---------------- validation ---------------- */

/** Validate raw field inputs before they are ever placed in a struct. */
bool sub_validate_imsi(const char *imsi);
bool sub_validate_msisdn(const char *msisdn);
bool sub_validate_name(const char *name);
const char *sub_status_to_str(sm_sub_status_t s);
sm_sub_status_t sub_status_from_str(const char *s);

/* ---------------- CRUD ---------------- */

/**
 * Add a new subscriber. subscriber_id is assigned by the DB (out param).
 * @return SM_OK, SM_ERR_DUPLICATE (imsi or msisdn already registered),
 *         SM_ERR_INVALID_ARG, SM_ERR_ALLOC_FAILED.
 */
sm_status_t subdb_add(sm_subscriber_db_t *db, const char *imsi, const char *msisdn,
                      const char *name, const char *region, const char *plan,
                      sm_sub_status_t status, uint64_t *out_id);

/** Delete a subscriber by ID. @return SM_OK or SM_ERR_NOT_FOUND. */
sm_status_t subdb_delete(sm_subscriber_db_t *db, uint64_t id);

/**
 * Update mutable fields of an existing subscriber. Pass NULL for any field
 * that should be left unchanged. IMSI/MSISDN are immutable identifiers and
 * are not updatable through this API (delete + add to change them).
 */
sm_status_t subdb_update(sm_subscriber_db_t *db, uint64_t id,
                         const char *name, const char *region,
                         const char *plan, const sm_sub_status_t *status);

/* ---------------- search ---------------- */

/**
 * All search functions return SM_OK with a heap-allocated array of pointers
 * to *snapshots* (copies) of matching subscribers in *out_array plus a count
 * in *out_count, so callers never hold direct pointers into the DB past the
 * lock. Caller must free with subdb_free_results().
 */
sm_status_t subdb_search_by_id(sm_subscriber_db_t *db, uint64_t id, sm_subscriber_t **out);
sm_status_t subdb_search_by_imsi(sm_subscriber_db_t *db, const char *imsi, sm_subscriber_t **out);
sm_status_t subdb_search_by_msisdn(sm_subscriber_db_t *db, const char *msisdn, sm_subscriber_t **out);

sm_status_t subdb_search_by_region(sm_subscriber_db_t *db, const char *region,
                                   sm_subscriber_t ***out_array, size_t *out_count);
sm_status_t subdb_search_by_plan(sm_subscriber_db_t *db, const char *plan,
                                 sm_subscriber_t ***out_array, size_t *out_count);
sm_status_t subdb_search_by_status(sm_subscriber_db_t *db, sm_sub_status_t status,
                                   sm_subscriber_t ***out_array, size_t *out_count);

/** List all subscribers (snapshot copies). */
sm_status_t subdb_list_all(sm_subscriber_db_t *db, sm_subscriber_t ***out_array, size_t *out_count);

/** Free a single snapshot returned by *_search_by_id/imsi/msisdn. */
void subdb_free_one(sm_subscriber_t *s);
/** Free an array of snapshots returned by list_all / search_by_region etc. */
void subdb_free_results(sm_subscriber_t **array, size_t count);

/* ---------------- sorting ---------------- */

typedef enum {
    SORT_BY_ID = 0,
    SORT_BY_REGION,
    SORT_BY_PLAN,
    SORT_BY_STATUS
} sm_sort_key_t;

/** Sort an array of snapshot pointers in place (ascending). Stable. */
void sub_sort_merge(sm_subscriber_t **array, size_t count, sm_sort_key_t key);
/** Sort an array of snapshot pointers in place (ascending). Not stable, in-place, avg O(n log n). */
void sub_sort_quick(sm_subscriber_t **array, size_t count, sm_sort_key_t key);

/* ---------------- persistence (CSV) ---------------- */

/** Atomically save the full DB to a CSV file (write to tmp + rename). */
sm_status_t subdb_save_csv(sm_subscriber_db_t *db, const char *path);
/** Load subscribers from a CSV file, adding them to (initially empty) db. */
sm_status_t subdb_load_csv(sm_subscriber_db_t *db, const char *path);
/** Backup = save_csv to a timestamped path under backup_dir. */
sm_status_t subdb_backup(sm_subscriber_db_t *db, const char *backup_dir, char *out_path, size_t out_path_len);
/** Restore = load_csv from an explicit backup file into an empty db. */
sm_status_t subdb_restore(sm_subscriber_db_t *db, const char *backup_path);

/** Total subscriber count. */
size_t subdb_count(sm_subscriber_db_t *db);

#ifdef __cplusplus
}
#endif
#endif /* SM_SUBSCRIBER_H */
