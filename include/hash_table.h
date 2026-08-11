/**
 * @file hash_table.h
 * @brief Generic thread-safe hash table: separate chaining, prime capacities,
 *        dynamic resize (grow-only, doubles-to-next-prime on load factor > 0.75).
 *
 * Thread safety: guarded by a single sm_rwlock_t. Readers (get, iterate)
 * take the read lock; writers (put, remove, resize) take the write lock.
 * Time complexity: O(1) average for get/put/remove, O(n) worst case under
 * pathological hash collisions, O(n) for resize (amortized O(1) per insert).
 */
#ifndef SM_HASH_TABLE_H
#define SM_HASH_TABLE_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t (*sm_hash_fn)(const void *key);
typedef bool     (*sm_key_eq_fn)(const void *a, const void *b);
typedef void     (*sm_free_fn)(void *ptr); /* pass NULL if the table does not own it */

typedef struct sm_ht_node {
    void               *key;
    void               *value;
    uint64_t            hash;
    struct sm_ht_node   *next;
} sm_ht_node_t;

typedef struct {
    sm_ht_node_t   **buckets;
    size_t            capacity;      /* always a prime number */
    size_t            count;
    sm_hash_fn        hash_fn;
    sm_key_eq_fn      key_eq_fn;
    sm_free_fn        free_key_fn;    /* may be NULL: caller owns keys   */
    sm_free_fn        free_value_fn;  /* may be NULL: caller owns values */
    sm_rwlock_t  lock;
    uint64_t          resize_count;
    uint64_t          collision_count;
} sm_hash_table_t;

/**
 * Create a hash table.
 * @param initial_capacity_hint  Suggested capacity (rounded up to a prime).
 * @param hash_fn      Required. Hash function over keys.
 * @param key_eq_fn    Required. Equality predicate over keys.
 * @param free_key_fn  Optional. Called on key when a node is removed/destroyed.
 * @param free_value_fn Optional. Called on value when a node is removed/destroyed.
 */
sm_status_t ht_create(size_t initial_capacity_hint,
                      sm_hash_fn hash_fn, sm_key_eq_fn key_eq_fn,
                      sm_free_fn free_key_fn, sm_free_fn free_value_fn,
                      sm_hash_table_t **out_table);

/** Destroy the table, freeing keys/values via the configured free functions. */
void ht_destroy(sm_hash_table_t *table);

/**
 * Insert or replace a key/value pair.
 * @return SM_OK on success, SM_ERR_DUPLICATE if replace_existing is false and
 *         the key already exists, SM_ERR_ALLOC_FAILED on OOM.
 */
sm_status_t ht_put(sm_hash_table_t *table, void *key, void *value, bool replace_existing);

/** Look up a value by key. @return SM_OK and *out_value set, or SM_ERR_NOT_FOUND. */
sm_status_t ht_get(sm_hash_table_t *table, const void *key, void **out_value);

/** Remove a key (frees it/its value via configured free functions). */
sm_status_t ht_remove(sm_hash_table_t *table, const void *key);

/** Current number of stored entries. */
size_t ht_size(sm_hash_table_t *table);

/**
 * Iterate every entry under the read lock. `cb` must not call back into
 * any mutating ht_* function on the same table (would deadlock).
 * Iteration stops early if `cb` returns false.
 */
typedef bool (*sm_ht_iter_fn)(void *key, void *value, void *user_data);
void ht_iterate(sm_hash_table_t *table, sm_ht_iter_fn cb, void *user_data);

/* ---- Ready-made hash/eq functions for common key types ---- */
uint64_t sm_hash_str(const void *key);      /* FNV-1a over a NUL-terminated string */
bool     sm_streq(const void *a, const void *b);
uint64_t sm_hash_u64(const void *key);       /* key points to a uint64_t */
bool     sm_u64eq(const void *a, const void *b);

#ifdef __cplusplus
}
#endif
#endif /* SM_HASH_TABLE_H */
