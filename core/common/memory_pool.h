#ifndef VP_MEMORY_POOL_H
#define VP_MEMORY_POOL_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/*
 * Fixed-size block pool.
 * Pre-allocates N blocks of a given size; acquire/release are O(1).
 * No malloc/free in the hot path.
 */
typedef struct {
    uint8_t *buffer;        /* contiguous backing memory */
    int     *free_stack;    /* stack of free block indices */
    int      top;           /* stack pointer */
    int      capacity;      /* total blocks */
    size_t   block_size;    /* bytes per block */
    int      in_use;        /* blocks currently acquired */
    size_t   total_bytes;   /* capacity * block_size */
} MemoryPool;

/* Create pool with `capacity` blocks of `block_size` bytes.
 * block_size is rounded up to 64-byte alignment. */
MemoryPool *memory_pool_create(int capacity, size_t block_size);

/* Acquire a block. Returns NULL if pool exhausted. */
void *memory_pool_acquire(MemoryPool *pool);

/* Release a block back to pool. */
void memory_pool_release(MemoryPool *pool, void *ptr);

/* Reset all blocks as free (bulk release). */
void memory_pool_reset(MemoryPool *pool);

/* Destroy pool and free backing memory. */
void memory_pool_destroy(MemoryPool *pool);

/* Stats */
int    memory_pool_in_use(const MemoryPool *pool);
int    memory_pool_available(const MemoryPool *pool);
size_t memory_pool_total_bytes(const MemoryPool *pool);

#endif /* VP_MEMORY_POOL_H */
