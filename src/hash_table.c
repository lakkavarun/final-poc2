/**
 * @file hash_table.c
 * @brief Separate-chaining hash table with prime capacities and rw-locking.
 */
#include "hash_table.h"
#include "logger.h"
#include <stdlib.h>
#include <string.h>

#define MODULE_NAME "HASHTABLE"
#define SM_HT_MAX_LOAD_FACTOR 0.75

/* ---------------- ready-made hash/eq functions ---------------- */

uint64_t sm_hash_str(const void *key)
{
    const unsigned char *s = (const unsigned char *)key;
    uint64_t hash = 1469598103934665603ULL; /* FNV-1a offset basis */
    while (s != NULL && *s != '\0') {
        hash ^= (uint64_t)(*s++);
        hash *= 1099511628211ULL; /* FNV prime */
    }
    return hash;
}

bool sm_streq(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b) == 0;
}

uint64_t sm_hash_u64(const void *key)
{
    uint64_t x = *(const uint64_t *)key;
    /* SplitMix64 finalizer: good avalanche for integer keys. */
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27; x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

bool sm_u64eq(const void *a, const void *b)
{
    return *(const uint64_t *)a == *(const uint64_t *)b;
}

/* ---------------- prime helpers ---------------- */

static bool is_prime(size_t n)
{
    if (n < 2U) return false;
    if (n % 2U == 0U) return n == 2U;
    for (size_t i = 3U; i * i <= n; i += 2U) {
        if (n % i == 0U) return false;
    }
    return true;
}

static size_t next_prime(size_t n)
{
    if (n < 5U) n = 5U;
    if (n % 2U == 0U) n++;
    while (!is_prime(n)) n += 2U;
    return n;
}

/* ---------------- lifecycle ---------------- */

sm_status_t ht_create(size_t initial_capacity_hint,
                      sm_hash_fn hash_fn, sm_key_eq_fn key_eq_fn,
                      sm_free_fn free_key_fn, sm_free_fn free_value_fn,
                      sm_hash_table_t **out_table)
{
    if (hash_fn == NULL || key_eq_fn == NULL || out_table == NULL) {
        return SM_ERR_NULL_PARAM;
    }
    sm_hash_table_t *t = (sm_hash_table_t *)calloc(1, sizeof(sm_hash_table_t));
    if (t == NULL) {
        return SM_ERR_ALLOC_FAILED;
    }
    t->capacity = next_prime(initial_capacity_hint);
    t->buckets  = (sm_ht_node_t **)calloc(t->capacity, sizeof(sm_ht_node_t *));
    if (t->buckets == NULL) {
        free(t);
        return SM_ERR_ALLOC_FAILED;
    }
    t->hash_fn       = hash_fn;
    t->key_eq_fn     = key_eq_fn;
    t->free_key_fn   = free_key_fn;
    t->free_value_fn = free_value_fn;
    if (sm_rwlock_init(&t->lock) != 0) {
        free(t->buckets);
        free(t);
        return SM_ERR_INTERNAL;
    }
    *out_table = t;
    return SM_OK;
}

void ht_destroy(sm_hash_table_t *table)
{
    if (table == NULL) {
        return;
    }
    (void)sm_rwlock_wrlock(&table->lock);
    for (size_t i = 0; i < table->capacity; ++i) {
        sm_ht_node_t *node = table->buckets[i];
        while (node != NULL) {
            sm_ht_node_t *next = node->next;
            if (table->free_key_fn != NULL)   table->free_key_fn(node->key);
            if (table->free_value_fn != NULL) table->free_value_fn(node->value);
            free(node);
            node = next;
        }
    }
    free(table->buckets);
    (void)sm_rwlock_unlock(&table->lock);
    sm_rwlock_destroy(&table->lock);
    free(table);
}

/* Caller must hold the write lock. */
static sm_status_t resize_locked(sm_hash_table_t *table, size_t new_capacity_hint)
{
    size_t new_capacity = next_prime(new_capacity_hint);
    sm_ht_node_t **new_buckets = (sm_ht_node_t **)calloc(new_capacity, sizeof(sm_ht_node_t *));
    if (new_buckets == NULL) {
        return SM_ERR_ALLOC_FAILED; /* table remains usable at old capacity */
    }
    for (size_t i = 0; i < table->capacity; ++i) {
        sm_ht_node_t *node = table->buckets[i];
        while (node != NULL) {
            sm_ht_node_t *next = node->next;
            size_t idx = (size_t)(node->hash % new_capacity);
            node->next = new_buckets[idx];
            new_buckets[idx] = node;
            node = next;
        }
    }
    free(table->buckets);
    table->buckets  = new_buckets;
    table->capacity = new_capacity;
    table->resize_count++;
    return SM_OK;
}

sm_status_t ht_put(sm_hash_table_t *table, void *key, void *value, bool replace_existing)
{
    if (table == NULL || key == NULL) {
        return SM_ERR_NULL_PARAM;
    }
    if (sm_rwlock_wrlock(&table->lock) != 0) {
        return SM_ERR_LOCK_FAILED;
    }

    uint64_t hash = table->hash_fn(key);
    size_t idx = (size_t)(hash % table->capacity);

    sm_ht_node_t *node = table->buckets[idx];
    while (node != NULL) {
        if (node->hash == hash && table->key_eq_fn(node->key, key)) {
            if (!replace_existing) {
                (void)sm_rwlock_unlock(&table->lock);
                return SM_ERR_DUPLICATE;
            }
            if (table->free_value_fn != NULL) table->free_value_fn(node->value);
            if (table->free_key_fn != NULL)   table->free_key_fn(key); /* new key redundant */
            else                                node->key = key;
            node->value = value;
            (void)sm_rwlock_unlock(&table->lock);
            return SM_OK;
        }
        node = node->next;
    }

    sm_ht_node_t *new_node = (sm_ht_node_t *)malloc(sizeof(sm_ht_node_t));
    if (new_node == NULL) {
        (void)sm_rwlock_unlock(&table->lock);
        return SM_ERR_ALLOC_FAILED;
    }
    new_node->key   = key;
    new_node->value = value;
    new_node->hash  = hash;
    new_node->next  = table->buckets[idx];
    if (table->buckets[idx] != NULL) {
        table->collision_count++;
    }
    table->buckets[idx] = new_node;
    table->count++;

    if ((double)table->count / (double)table->capacity > SM_HT_MAX_LOAD_FACTOR) {
        sm_status_t rst = resize_locked(table, table->capacity * 2U);
        if (rst != SM_OK) {
            SM_LOG_WARN(MODULE_NAME, "resize failed, continuing at capacity=%llu (degraded perf)",
                        (unsigned long long)table->capacity);
        }
    }

    (void)sm_rwlock_unlock(&table->lock);
    return SM_OK;
}

sm_status_t ht_get(sm_hash_table_t *table, const void *key, void **out_value)
{
    if (table == NULL || key == NULL || out_value == NULL) {
        return SM_ERR_NULL_PARAM;
    }
    if (sm_rwlock_rdlock(&table->lock) != 0) {
        return SM_ERR_LOCK_FAILED;
    }
    uint64_t hash = table->hash_fn(key);
    size_t idx = (size_t)(hash % table->capacity);
    sm_ht_node_t *node = table->buckets[idx];
    while (node != NULL) {
        if (node->hash == hash && table->key_eq_fn(node->key, key)) {
            *out_value = node->value;
            (void)sm_rwlock_unlock(&table->lock);
            return SM_OK;
        }
        node = node->next;
    }
    (void)sm_rwlock_unlock(&table->lock);
    return SM_ERR_NOT_FOUND;
}

sm_status_t ht_remove(sm_hash_table_t *table, const void *key)
{
    if (table == NULL || key == NULL) {
        return SM_ERR_NULL_PARAM;
    }
    if (sm_rwlock_wrlock(&table->lock) != 0) {
        return SM_ERR_LOCK_FAILED;
    }
    uint64_t hash = table->hash_fn(key);
    size_t idx = (size_t)(hash % table->capacity);
    sm_ht_node_t *node = table->buckets[idx];
    sm_ht_node_t *prev = NULL;
    while (node != NULL) {
        if (node->hash == hash && table->key_eq_fn(node->key, key)) {
            if (prev != NULL) prev->next = node->next;
            else               table->buckets[idx] = node->next;
            if (table->free_key_fn != NULL)   table->free_key_fn(node->key);
            if (table->free_value_fn != NULL) table->free_value_fn(node->value);
            free(node);
            table->count--;
            (void)sm_rwlock_unlock(&table->lock);
            return SM_OK;
        }
        prev = node;
        node = node->next;
    }
    (void)sm_rwlock_unlock(&table->lock);
    return SM_ERR_NOT_FOUND;
}

size_t ht_size(sm_hash_table_t *table)
{
    if (table == NULL) {
        return 0U;
    }
    (void)sm_rwlock_rdlock(&table->lock);
    size_t n = table->count;
    (void)sm_rwlock_unlock(&table->lock);
    return n;
}

void ht_iterate(sm_hash_table_t *table, sm_ht_iter_fn cb, void *user_data)
{
    if (table == NULL || cb == NULL) {
        return;
    }
    (void)sm_rwlock_rdlock(&table->lock);
    for (size_t i = 0; i < table->capacity; ++i) {
        sm_ht_node_t *node = table->buckets[i];
        while (node != NULL) {
            if (!cb(node->key, node->value, user_data)) {
                (void)sm_rwlock_unlock(&table->lock);
                return;
            }
            node = node->next;
        }
    }
    (void)sm_rwlock_unlock(&table->lock);
}
