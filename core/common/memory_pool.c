#include "memory_pool.h"
#include <stdlib.h>
#include <string.h>

#define ALIGN_UP(x, a) (((x) + (a) - 1) & ~((a) - 1))

MemoryPool *memory_pool_create(int capacity, size_t block_size) {
    if (capacity <= 0 || block_size == 0) return NULL;

    MemoryPool *pool = calloc(1, sizeof(MemoryPool));
    if (!pool) return NULL;

    pool->block_size = ALIGN_UP(block_size, 64);
    pool->capacity   = capacity;
    pool->total_bytes = (size_t)capacity * pool->block_size;

    pool->buffer = aligned_alloc(64, pool->total_bytes);
    if (!pool->buffer) { free(pool); return NULL; }

    pool->free_stack = malloc(sizeof(int) * capacity);
    if (!pool->free_stack) { free(pool->buffer); free(pool); return NULL; }

    /* push all indices onto free stack */
    for (int i = 0; i < capacity; i++) {
        pool->free_stack[i] = capacity - 1 - i;
    }
    pool->top    = capacity;
    pool->in_use = 0;

    memset(pool->buffer, 0, pool->total_bytes);
    return pool;
}

void *memory_pool_acquire(MemoryPool *pool) {
    if (!pool || pool->top <= 0) return NULL;
    int idx = pool->free_stack[--pool->top];
    pool->in_use++;
    return pool->buffer + (size_t)idx * pool->block_size;
}

void memory_pool_release(MemoryPool *pool, void *ptr) {
    if (!pool || !ptr) return;
    size_t offset = (uint8_t *)ptr - pool->buffer;
    int idx = (int)(offset / pool->block_size);
    if (idx < 0 || idx >= pool->capacity) return;
    pool->free_stack[pool->top++] = idx;
    pool->in_use--;
}

void memory_pool_reset(MemoryPool *pool) {
    if (!pool) return;
    for (int i = 0; i < pool->capacity; i++) {
        pool->free_stack[i] = pool->capacity - 1 - i;
    }
    pool->top    = pool->capacity;
    pool->in_use = 0;
}

void memory_pool_destroy(MemoryPool *pool) {
    if (!pool) return;
    free(pool->free_stack);
    free(pool->buffer);
    free(pool);
}

int memory_pool_in_use(const MemoryPool *pool) {
    return pool ? pool->in_use : 0;
}

int memory_pool_available(const MemoryPool *pool) {
    return pool ? (pool->capacity - pool->in_use) : 0;
}

size_t memory_pool_total_bytes(const MemoryPool *pool) {
    return pool ? pool->total_bytes : 0;
}
