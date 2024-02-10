#include "cache.h"
#include "internal_types.h"

/* debugging and assertions */
#include <assert.h>
#if !defined(NDEBUG)
#include <string.h>  /* for memset */
#define TABLE_CACHE_DEBUG_CHUNK_INITIALIZER 0x41
#define TABLE_CACHE_DEBUG_FREELIST_INITIALIZER 0x42
#endif

ptrdiff_t hamt_table_cache_config_default_bucket_count = 32;
ptrdiff_t hamt_table_cache_default_bucket_sizes[32] = {
    10000, 338900, 220200, 155800, 86700, 39500, 15000, 4900, 4900, 5200, 5000,
    4900,  4700,   4600,   4600,   4600,  4200,  4600,  4700, 4300, 4600, 4800,
    4500,  5100,   5100,   5300,   5500,  5900,  7000,  8000, 9900, 6900};

#if defined(WITH_TABLE_CACHE_STATS)
struct table_allocator_stats {
    size_t alloc_count;
    size_t free_count;
};
#endif

struct table_allocator_freelist {
    struct table_allocator_freelist *next;
};

struct table_allocator_chunk {
    struct table_allocator_chunk *next; /* pointer to next chunk */
    ptrdiff_t size;                     /* buffer size in bytes */
    struct hamt_node *buf;              /* pointer to the actual buffer */
};

struct table_allocator {
    struct table_allocator_chunk *chunk; /* backing buffer (chain of chunks) */
    ptrdiff_t size;                      /* count of allocated tables */
    ptrdiff_t buf_ix;                    /* high water mark in current chunk */
    size_t chunk_count;                  /* number of chunks in the pool */
    ptrdiff_t table_size;                /* number of rows in table */
    struct table_allocator_freelist *fl; /* head of the free list */
#if defined(WITH_TABLE_CACHE_STATS)
    struct table_allocator_stats stats; /* statistics */
#endif
};

struct hamt_table_cache {
    struct table_allocator pools[32];
    struct hamt_allocator *backing_allocator;
};

int table_allocator_create(struct table_allocator *pool,
                           ptrdiff_t initial_cache_size, ptrdiff_t table_size,
                           struct hamt_allocator *backing_allocator)
{
    /* pool config */
    *pool = (struct table_allocator){.chunk = NULL,
                                     .size = 0,
                                     .buf_ix = 0,
                                     .chunk_count = 1,
                                     .table_size = table_size,
                                     .fl = NULL};
    /* set up initial chunk */
    pool->chunk = backing_allocator->malloc(
        sizeof(struct table_allocator_chunk), backing_allocator->ctx);
    if (!pool->chunk)
        goto err_no_cleanup;
    pool->chunk->size = initial_cache_size * table_size;
    pool->chunk->buf = (struct hamt_node *)backing_allocator->malloc(
        pool->chunk->size * sizeof(struct hamt_node), backing_allocator->ctx);
    if (!pool->chunk->buf)
        goto err_free_chunk;
    pool->chunk->next = NULL;
#if defined(WITH_TABLE_CACHE_STATS)
    /* set up stats storage */
    pool->stats =
        (struct table_allocator_stats){.alloc_count = 0, .free_count = 0};
#endif
    return 0;
err_free_chunk:
    backing_allocator->free(pool->chunk, sizeof(struct table_allocator_chunk),
                            backing_allocator->ctx);
    pool->chunk = NULL;
err_no_cleanup:
    pool->chunk_count = 0;
    return -1;
}

void table_allocator_delete(struct table_allocator *pool,
                            struct hamt_allocator *backing_allocator)
{
    struct table_allocator_chunk *current_chunk = pool->chunk, *next_chunk;
    /* free all buffers in all chunks */
    while (current_chunk) {
        backing_allocator->free(current_chunk->buf,
                                current_chunk->size * sizeof(struct hamt_node),
                                backing_allocator->ctx);
        next_chunk = current_chunk->next;
        backing_allocator->free(current_chunk,
                                sizeof(struct table_allocator_chunk),
                                backing_allocator->ctx);
        current_chunk = next_chunk;
    }
}

/**
 * Return a pointer to a hamt_node array of size table_size.
 */
struct hamt_node *
table_allocator_alloc(struct table_allocator *pool,
                      struct hamt_allocator *backing_allocator)
{
#if defined(WITH_TABLE_CACHE_STATS)
    pool->stats.alloc_count++;
#endif
    /* attempt to return from the freelist */
    if (pool->fl) {
        struct table_allocator_freelist *f = pool->fl;
        pool->fl = pool->fl->next;
# if !defined(NDEBUG)
        /* debug: clear the pointer info with known value */
        memset(f, TABLE_CACHE_DEBUG_FREELIST_INITIALIZER, sizeof(struct hamt_node*));
# endif
        return (struct hamt_node *)f;
    }
    /* freelist is empty, serve from chunk */
    if (pool->buf_ix == pool->chunk->size) {
        /* if chunk has no capacity left, create new one */
        struct table_allocator_chunk *chunk = backing_allocator->malloc(
            sizeof(struct table_allocator_chunk), backing_allocator->ctx);
        if (!chunk)
            goto err_no_cleanup;
        /* double size of new chunk compared to previous */
        chunk->size = pool->chunk->size * 2;
        chunk->buf = (struct hamt_node *)backing_allocator->malloc(
            chunk->size * sizeof(struct hamt_node), backing_allocator->ctx);
        if (!chunk->buf)
            goto err_free_chunk;
#if !defined(NDEBUG)
        /* debug: initialize the chunk to known values */
        memset(chunk->buf, TABLE_CACHE_DEBUG_CHUNK_INITIALIZER,
               chunk->size * sizeof(struct hamt_node));
#endif
        chunk->next = pool->chunk;
        pool->chunk = chunk;
        pool->buf_ix = 0;
        pool->chunk_count++;
    }
    /* serve from chunk */
    struct hamt_node *p = &pool->chunk->buf[pool->buf_ix];
    pool->buf_ix += pool->table_size;
    pool->size++;
    return p;
err_free_chunk:
    backing_allocator->free(pool->chunk, sizeof(struct table_allocator_chunk),
                            backing_allocator->ctx);
    pool->chunk = NULL;
err_no_cleanup:
    return NULL;
}

void table_allocator_free(struct table_allocator *pool, void *p)
{
#if defined(WITH_TABLE_CACHE_STATS)
    pool->stats.free_count++;
#endif
#ifndef NDEBUG
    /* debug: re-initialize table content to known initializer */
    memset(p, TABLE_CACHE_DEBUG_FREELIST_INITIALIZER,
           pool->table_size * sizeof(struct hamt_node));
#endif
    /* insert returned memory at the front of the freelist */
    struct table_allocator_freelist *head =
        (struct table_allocator_freelist *)p;
    head->next = pool->fl;
    pool->fl = head;
}

struct hamt_table_cache *
hamt_table_cache_create(struct hamt_table_cache_config *cfg)
{
    struct hamt_table_cache *cache = cfg->backing_allocator->malloc(
        sizeof *cache, cfg->backing_allocator->ctx);
    if (cache) {
        cache->backing_allocator = cfg->backing_allocator;
        for (ptrdiff_t i = 0; i < cfg->bucket_count; ++i) {
            table_allocator_create(&cache->pools[i],
                                   cfg->initial_bucket_sizes[i], i + 1,
                                   cfg->backing_allocator);
        }
    }
    return cache;
}

void hamt_table_cache_delete(struct hamt_table_cache *cache)
{
    for (size_t i = 0; i < 32; ++i) {
        table_allocator_delete(&cache->pools[i], cache->backing_allocator);
    }
    cache->backing_allocator = NULL;
}

struct hamt_node *hamt_table_cache_alloc(struct hamt_table_cache *cache,
                                         size_t n)
{
    assert(n > 0 && "Request for zero-size allocation");
    assert(n < 33 && "Request for >32 rows allocation");
    return table_allocator_alloc(&cache->pools[n - 1],
                                 cache->backing_allocator);
}

void hamt_table_cache_free(struct hamt_table_cache *cache, size_t n, void *p)
{
    assert(n > 0 && "Request for zero-size free");
    assert(n < 33 && "Request for >32 rows free");
    table_allocator_free(&cache->pools[n-1], p);
}
