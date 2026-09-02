/**
 * @file memory_pool.c
 * @brief Slab-based fixed-size memory pool implementation.
 */
#include "memory_pool.h"
#include "logger.h"
#include <stdlib.h>
#include <string.h>

#define MODULE_NAME "MEMPOOL"

static sm_status_t grow_pool(sm_memory_pool_t *pool)
{
    sm_pool_slab_t *slab = (sm_pool_slab_t *)malloc(sizeof(sm_pool_slab_t));
    if (slab == NULL) {
        return SM_ERR_ALLOC_FAILED;
    }
    size_t bytes = pool->block_size * pool->blocks_per_slab;
    slab->memory = (uint8_t *)malloc(bytes);
    if (slab->memory == NULL) {
        free(slab);
        return SM_ERR_ALLOC_FAILED;
    }

    /* Thread onto free list. */
    for (size_t i = 0; i < pool->blocks_per_slab; ++i) {
        sm_pool_block_t *blk = (sm_pool_block_t *)(void *)(slab->memory + (i * pool->block_size));
        blk->next = pool->free_list;
        pool->free_list = blk;
    }

    slab->next = pool->slabs;
    pool->slabs = slab;
    pool->total_blocks += pool->blocks_per_slab;
    return SM_OK;
}

sm_status_t mempool_create(size_t block_size, size_t blocks_per_slab,
                            sm_memory_pool_t **out_pool)
{
    if (out_pool == NULL) {
        return SM_ERR_NULL_PARAM;
    }
    if (block_size < sizeof(void *) || blocks_per_slab == 0U) {
        return SM_ERR_INVALID_ARG;
    }

    sm_memory_pool_t *pool = (sm_memory_pool_t *)calloc(1, sizeof(sm_memory_pool_t));
    if (pool == NULL) {
        return SM_ERR_ALLOC_FAILED;
    }
    pool->block_size      = block_size;
    pool->blocks_per_slab = blocks_per_slab;
    if (sm_mutex_init(&pool->lock) != 0) {
        free(pool);
        return SM_ERR_INTERNAL;
    }

    sm_status_t st = grow_pool(pool);
    if (st != SM_OK) {
        sm_mutex_destroy(&pool->lock);
        free(pool);
        return st;
    }

    *out_pool = pool;
    SM_LOG_INFO(MODULE_NAME, "pool created block_size=%llu blocks_per_slab=%llu",
                (unsigned long long)block_size, (unsigned long long)blocks_per_slab);
    return SM_OK;
}

void mempool_destroy(sm_memory_pool_t *pool)
{
    if (pool == NULL) {
        return;
    }
    (void)sm_mutex_lock(&pool->lock);
    sm_pool_slab_t *slab = pool->slabs;
    while (slab != NULL) {
        sm_pool_slab_t *next = slab->next;
        free(slab->memory);
        free(slab);
        slab = next;
    }
    (void)sm_mutex_unlock(&pool->lock);
    sm_mutex_destroy(&pool->lock);
    free(pool);
}

void *mempool_alloc(sm_memory_pool_t *pool)
{
    if (pool == NULL) {
        return NULL;
    }
    if (sm_mutex_lock(&pool->lock) != 0) {
        return NULL;
    }

    if (pool->free_list == NULL) {
        if (grow_pool(pool) != SM_OK) {
            pool->alloc_failures++;
            (void)sm_mutex_unlock(&pool->lock);
            SM_LOG_ERROR(MODULE_NAME, "pool exhausted, growth failed (block_size=%llu)",
                         (unsigned long long)pool->block_size);
            return NULL;
        }
    }

    sm_pool_block_t *blk = pool->free_list;
    pool->free_list = blk->next;
    pool->used_blocks++;
    pool->alloc_count++;
    if (pool->used_blocks > pool->peak_blocks) {
        pool->peak_blocks = pool->used_blocks;
    }
    (void)sm_mutex_unlock(&pool->lock);

    memset(blk, 0, pool->block_size); /* prevent info leak from reused memory */
    return (void *)blk;
}

void mempool_free(sm_memory_pool_t *pool, void *ptr)
{
    if (pool == NULL || ptr == NULL) {
        return;
    }
    (void)sm_mutex_lock(&pool->lock);
    /* Defensive: poison memory before returning it to catch use-after-free
     * during testing (pattern 0xDD is a common debug-heap convention). */
    memset(ptr, 0xDD, pool->block_size);
    sm_pool_block_t *blk = (sm_pool_block_t *)ptr;
    blk->next = pool->free_list;
    pool->free_list = blk;
    if (pool->used_blocks > 0U) {
        pool->used_blocks--;
    }
    pool->free_count++;
    (void)sm_mutex_unlock(&pool->lock);
}

sm_status_t mempool_get_stats(sm_memory_pool_t *pool, sm_pool_stats_t *out)
{
    if (pool == NULL || out == NULL) {
        return SM_ERR_NULL_PARAM;
    }
    (void)sm_mutex_lock(&pool->lock);
    out->block_size      = pool->block_size;
    out->total_blocks     = pool->total_blocks;
    out->used_blocks      = pool->used_blocks;
    out->peak_blocks      = pool->peak_blocks;
    out->alloc_count       = pool->alloc_count;
    out->free_count        = pool->free_count;
    out->alloc_failures    = pool->alloc_failures;
    (void)sm_mutex_unlock(&pool->lock);
    return SM_OK;
}
