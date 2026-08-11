/**
 * @file memory_pool.h
 * @brief Fixed-block-size memory pool (slab allocator) with free-list reuse.
 *
 * Rationale: subscriber records and hash nodes are allocated/freed at very
 * high frequency. A slab allocator avoids malloc/free overhead and heap
 * fragmentation, and lets us report peak usage / allocation statistics.
 *
 * Thread safety: all public functions are thread-safe (internally
 * mutex-protected). Time complexity: alloc/free are O(1) amortized.
 */
#ifndef SM_MEMORY_POOL_H
#define SM_MEMORY_POOL_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sm_pool_block {
    struct sm_pool_block *next; /* intrusive free-list link */
} sm_pool_block_t;

typedef struct sm_pool_slab {
    struct sm_pool_slab *next;
    uint8_t              *memory;
} sm_pool_slab_t;

typedef struct {
    size_t            block_size;      /* user-visible block size (rounded up)   */
    size_t            blocks_per_slab;
    sm_pool_block_t  *free_list;
    sm_pool_slab_t   *slabs;
    size_t            total_blocks;
    size_t            used_blocks;
    size_t            peak_blocks;
    uint64_t          alloc_count;
    uint64_t          free_count;
    uint64_t          alloc_failures;
    sm_mutex_t   lock;
} sm_memory_pool_t;

/**
 * Create a pool serving fixed-size blocks.
 * @param block_size      Size in bytes of each block (min sizeof(void*)).
 * @param blocks_per_slab  How many blocks to carve out per underlying malloc.
 * @param out_pool        Receives the newly allocated pool.
 * @return SM_OK, SM_ERR_NULL_PARAM, SM_ERR_INVALID_ARG, SM_ERR_ALLOC_FAILED.
 */
sm_status_t mempool_create(size_t block_size, size_t blocks_per_slab,
                            sm_memory_pool_t **out_pool);

/** Destroy a pool and release all underlying slabs. Safe to call with NULL. */
void mempool_destroy(sm_memory_pool_t *pool);

/**
 * Acquire one block. Grows the pool (new slab) if the free list is empty.
 * @return pointer to a block_size-byte region, or NULL on allocation failure.
 */
void *mempool_alloc(sm_memory_pool_t *pool);

/** Return a block to the pool. `ptr` must have come from mempool_alloc(pool). */
void mempool_free(sm_memory_pool_t *pool, void *ptr);

/** Snapshot of pool statistics, useful for audit / capacity-planning reports. */
typedef struct {
    size_t   block_size;
    size_t   total_blocks;
    size_t   used_blocks;
    size_t   peak_blocks;
    uint64_t alloc_count;
    uint64_t free_count;
    uint64_t alloc_failures;
} sm_pool_stats_t;

/** Fill `out` with a consistent snapshot of the pool's counters. */
sm_status_t mempool_get_stats(sm_memory_pool_t *pool, sm_pool_stats_t *out);

#ifdef __cplusplus
}
#endif
#endif /* SM_MEMORY_POOL_H */
