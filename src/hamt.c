#include "hamt.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/mman.h>

/* #include "mem.h" */

/* Pointer tagging */
#define HAMT_TAG_MASK 0x3 /* last two bits */
#define HAMT_TAG_VALUE 0x1
#define tagged(__p) (struct hamt_node *)((uintptr_t)__p | HAMT_TAG_VALUE)
#define untagged(__p) (struct hamt_node *)((uintptr_t)__p & ~HAMT_TAG_MASK)
#define is_value(__p) (((uintptr_t)__p & HAMT_TAG_MASK) == HAMT_TAG_VALUE)

/* Bit fiddling */
#define index_clear_bit(_index, _n) _index & ~(1 << _n)
#define index_set_bit(_index, _n) _index | (1 << _n)

/* Node data structure */
#define TABLE(a) a->as.table.ptr
#define INDEX(a) a->as.table.index
#define VALUE(a) a->as.kv.value
#define KEY(a) a->as.kv.key

/* Memory management */
#define mem_alloc(ator, size) (ator)->malloc(size, (ator)->ctx)
#define mem_realloc(ator, ptr, size_old, size_new)                             \
    (ator)->realloc(ptr, size_old, size_new, (ator)->ctx)
#define mem_free(ator, ptr, size) (ator)->free(ptr, size, (ator)->ctx)

/* Default allocator uses system malloc */
static void *stdlib_malloc(const ptrdiff_t size, void *ctx)
{
    (void)ctx;
    return malloc(size);
}

static void *stdlib_realloc(void *ptr, const ptrdiff_t old_size,
                            const ptrdiff_t new_size, void *ctx)
{
    (void)ctx;
    (void)old_size;
    return realloc(ptr, new_size);
}

void stdlib_free(void *ptr, const ptrdiff_t size, void *ctx)
{
    (void)ctx;
    (void)size;
    free(ptr);
}

struct hamt_allocator hamt_allocator_default = {stdlib_malloc, stdlib_realloc,
                                                stdlib_free};

struct hamt_node {
    union {
        struct {
            void *value; /* tagged pointer */
            void *key;
        } kv;
        struct {
            struct hamt_node *ptr;
            uint32_t index;
        } table;
    } as;
};

#if defined(WITH_TABLE_CACHE)
struct table_allocator;
struct hamt_cache_config hamt_cache_config_default = {
    .initial_cache_sizes = { 
        10000, 338900, 220200, 155800, 86700, 39500, 15000, 4900, 4900, 5200,
        5000, 4900, 4700, 4600, 4600, 4600, 4200, 4600, 4700, 4300, 4600, 4800,
        4500, 5100, 5100, 5300, 5500, 5900, 7000, 8000, 9900, 6900 }
};
#endif

struct hamt {
    struct hamt_node *root;
    size_t size;
    hamt_key_hash_fn key_hash;
    hamt_cmp_fn key_cmp;
    struct hamt_allocator *ator;
#if defined(WITH_TABLE_CACHE)
    struct hamt_table_cache_32 *table_ator;
#endif
};

/* hashing w/ state management */
struct hash_state {
    const void *key;
    hamt_key_hash_fn hash_fn;
    uint32_t hash;
    size_t depth;
    size_t shift;
};

/* Search results */
typedef enum {
    SEARCH_SUCCESS,
    SEARCH_FAIL_NOTFOUND,
    SEARCH_FAIL_KEYMISMATCH
} search_status;

struct search_result {
    search_status status;
    struct hamt_node *anchor;
    struct hamt_node *value;
    struct hash_state *hash;
};

/* Removal results */
typedef enum { REMOVE_SUCCESS, REMOVE_GATHERED, REMOVE_NOTFOUND } remove_status;

struct remove_result {
    remove_status status;
    void *value;
};

struct path_result {
    union {
        struct search_result sr;
        struct remove_result rr;
    };
    struct hamt_node *root;
};

static inline struct hash_state *hash_next(struct hash_state *h)
{
    h->depth += 1;
    h->shift += 5;
    if (h->shift > 25) {
        h->hash = h->hash_fn(h->key, h->depth);
        h->shift = 0;
    }
    return h;
}

static inline uint32_t hash_get_index(const struct hash_state *h)
{
    return (h->hash >> h->shift) & 0x1f;
}

static int get_popcount(uint32_t n) { return __builtin_popcount(n); }

static int get_pos(uint32_t sparse_index, uint32_t bitmap)
{
    return get_popcount(bitmap & ((1 << sparse_index) - 1));
}

static inline bool has_index(const struct hamt_node *anchor, size_t index)
{
    assert(anchor && "anchor must not be NULL");
    assert(index < 32 && "index must not be larger than 31");
    return INDEX(anchor) & (1 << index);
}

/*
 * Table memory management
 *
 * Table memory is managed using an array of fixed-size pool allocators with a
 * freelist.
 */

#if defined(WITH_TABLE_CACHE)

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
    ptrdiff_t size;                     /* buffer size in count */
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

struct hamt_table_cache_32 {
    struct table_allocator pools[32];
    ptrdiff_t ref_count;
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
    /* insert returned memory at the front of the freelist */
    struct table_allocator_freelist *head =
        (struct table_allocator_freelist *)p;
    head->next = pool->fl;
    pool->fl = head;
}

void hamt_table_cache_init(struct hamt_table_cache_32 **cache_addr,
                           struct hamt_allocator *backing_allocator)
{
    struct hamt_table_cache_32* cache = mem_alloc(backing_allocator, sizeof(*cache));
    for (size_t i = 0; i < 32; ++i) {
        table_allocator_create(&cache->pools[i],
                               hamt_cache_config_default.initial_cache_sizes[i],
                               i + 1, backing_allocator);
    }
    cache->ref_count = 1;
    *cache_addr = cache;
}

void hamt_table_cache_destroy(struct hamt_table_cache_32 *cache,
                              struct hamt_allocator *backing_allocator)
{
    if (--cache->ref_count == 0) {
        for (size_t i = 0; i < 32; ++i) {
            table_allocator_delete(&cache->pools[i], backing_allocator);
        }
        mem_free(backing_allocator, cache, sizeof(*cache));
    }

}
#endif /* if defined(WITH_TABLE_CACHE) */

struct hamt_node *table_allocate(const struct hamt *h, size_t size)
{
    if (size == 0)
        return NULL;
#if defined(WITH_TABLE_CACHE)
    return table_allocator_alloc(&h->table_ator->pools[size - 1], h->ator);
#else
    return mem_alloc(h->ator, size * sizeof(struct hamt_node));
#endif
}

void table_free(struct hamt *h, struct hamt_node *ptr, size_t n_rows)
{
    if (ptr && n_rows)
#if defined(WITH_TABLE_CACHE)
        table_allocator_free(&h->table_ator->pools[n_rows - 1], ptr);
#else
        mem_free(h->ator, ptr, n_rows * sizeof(struct hamt_node));
#endif
}

struct hamt_node *table_extend(struct hamt *h, struct hamt_node *anchor,
                               size_t n_rows, uint32_t index, uint32_t pos)
{
    struct hamt_node *new_table = table_allocate(h, n_rows + 1);
    if (!new_table)
        return NULL;
    if (n_rows > 0) {
        /* copy over table */
        memcpy(&new_table[0], &TABLE(anchor)[0],
               pos * sizeof(struct hamt_node));
        /* note: this works since (n_rows - pos) == 0 for cases
         * where we're adding the new k/v pair at the end (i.e. memcpy(a, b, 0)
         * is a nop) */
        memcpy(&new_table[pos + 1], &TABLE(anchor)[pos],
               (n_rows - pos) * sizeof(struct hamt_node));
    }
    assert(!is_value(VALUE(anchor)) && "URGS");
    table_free(h, TABLE(anchor), n_rows);
    TABLE(anchor) = new_table;
    INDEX(anchor) |= (1 << index);
    return anchor;
}

struct hamt_node *table_shrink(struct hamt *h, struct hamt_node *anchor,
                               size_t n_rows, uint32_t index, uint32_t pos)
{
    /* debug assertions */
    assert(anchor && "Anchor cannot be NULL");
    assert(!is_value(VALUE(anchor)) &&
           "Invariant: shrinking a table requires an internal node");

    struct hamt_node *new_table = NULL;
    uint32_t new_index = 0;
    if (n_rows > 1) {
        new_table = table_allocate(h, n_rows - 1);
        if (!new_table)
            return NULL; /* mem allocation error */
        new_index = INDEX(anchor) & ~(1 << index);
        memcpy(&new_table[0], &TABLE(anchor)[0],
               pos * sizeof(struct hamt_node));
        memcpy(&new_table[pos], &TABLE(anchor)[pos + 1],
               (n_rows - pos - 1) * sizeof(struct hamt_node));
    }
    table_free(h, TABLE(anchor), n_rows);
    INDEX(anchor) = new_index;
    TABLE(anchor) = new_table;
    return anchor;
}

struct hamt_node *table_gather(struct hamt *h, struct hamt_node *anchor,
                               uint32_t pos)
{
    /* debug assertions */
    assert(anchor && "Anchor cannot be NULL");
    assert(!is_value(VALUE(anchor)) &&
           "Invariant: gathering a table requires an internal anchor");
    assert((pos == 0 || pos == 1) && "pos must be 0 or 1");

    int n_rows = get_popcount(INDEX(anchor));
    assert((n_rows == 2 || n_rows == 1) &&
           "Table must have size 1 or 2 to gather");
    struct hamt_node *table = TABLE(anchor);
    KEY(anchor) = table[pos].as.kv.key;
    VALUE(anchor) = table[pos].as.kv.value; /* already tagged */
    table_free(h, table, n_rows);
    return anchor;
}

struct hamt_node *table_dup(const struct hamt *h, struct hamt_node *anchor)
{
    int n_rows = get_popcount(INDEX(anchor));
    struct hamt_node *new_table = table_allocate(h, n_rows);
    if (new_table) {
        memcpy(&new_table[0], &TABLE(anchor)[0],
               n_rows * sizeof(struct hamt_node));
    }
    return new_table;
}

struct hamt *hamt_create(hamt_key_hash_fn key_hash, hamt_cmp_fn key_cmp,
                         struct hamt_allocator *ator)
{
    struct hamt *h = mem_alloc(ator, sizeof(struct hamt));
    h->ator = ator;
    h->root = mem_alloc(ator, sizeof(struct hamt_node));
    memset(h->root, 0, sizeof(struct hamt_node));
    h->size = 0;
    h->key_hash = key_hash;
    h->key_cmp = key_cmp;
#if defined(WITH_TABLE_CACHE)
    hamt_table_cache_init(&h->table_ator, h->ator);
#endif
    return h;
}

/**
 * Create a (shallow) duplicate of the HAMT.
 *
 * @param h A hash-array mapped trie instance
 * @return A pointer to a shallow copy of h
 */
static struct hamt *hamt_copy_shallow(const struct hamt *h)
{
    struct hamt *copy = mem_alloc(h->ator, sizeof(struct hamt));
    if (copy) {
        /* make a shallow copy of the root */
        *copy = (struct hamt) {
            .ator = h->ator,
            .key_hash = h->key_hash,
            .key_cmp = h->key_cmp,
            .root = h->root,
            .size = h->size,
#if defined(WITH_TABLE_CACHE)
            .table_ator = h->table_ator
#endif
        };
#if defined(WITH_TABLE_CACHE)
        h->table_ator->ref_count++;
#endif
    }
    return copy;
}

static const struct hamt_node *insert_kv(struct hamt *h,
                                         struct hamt_node *anchor,
                                         struct hash_state *hash, void *key,
                                         void *value)
{
    /* calculate position in new table */
    uint32_t ix = hash_get_index(hash);
    uint32_t new_index = INDEX(anchor) | (1 << ix);
    int pos = get_pos(ix, new_index);
    /* extend table */
    size_t n_rows = get_popcount(INDEX(anchor));
    anchor = table_extend(h, anchor, n_rows, ix, pos);
    if (!anchor)
        return NULL;
    struct hamt_node *new_table = TABLE(anchor);
    /* set new k/v pair */
    new_table[pos].as.kv.key = key;
    new_table[pos].as.kv.value = tagged(value);
    /* return a pointer to the inserted k/v pair */
    return &new_table[pos];
}

static const struct hamt_node *insert_table(struct hamt *h,
                                            struct hamt_node *anchor,
                                            struct hash_state *hash, void *key,
                                            void *value)
{
    /* FIXME: check for alloc failure and bail out correctly (deleting the
     *        incomplete subtree */

    /* Collect everything we know about the existing value */
    struct hash_state *x_hash = &(struct hash_state){
        .key = KEY(anchor),
        .hash_fn = hash->hash_fn,
        .hash = hash->hash_fn(KEY(anchor), hash->depth / 5),
        .depth = hash->depth,
        .shift = hash->shift};
    void *x_value = VALUE(anchor); /* tagged (!) value ptr */
    /* increase depth until the hashes diverge, building a list
     * of tables along the way */
    struct hash_state *next_hash = hash_next(hash);
    struct hash_state *x_next_hash = hash_next(x_hash);
    uint32_t next_index = hash_get_index(next_hash);
    uint32_t x_next_index = hash_get_index(x_next_hash);
    while (x_next_index == next_index) {
        TABLE(anchor) = table_allocate(h, 1);
        INDEX(anchor) = (1 << next_index);
        next_hash = hash_next(next_hash);
        x_next_hash = hash_next(x_next_hash);
        next_index = hash_get_index(next_hash);
        x_next_index = hash_get_index(x_next_hash);
        anchor = TABLE(anchor);
    }
    /* the hashes are different, let's allocate a table with two
     * entries to store the existing and new values */
    TABLE(anchor) = table_allocate(h, 2);
    INDEX(anchor) = (1 << next_index) | (1 << x_next_index);
    /* determine the proper position in the allocated table */
    int x_pos = get_pos(x_next_index, INDEX(anchor));
    int pos = get_pos(next_index, INDEX(anchor));
    /* fill in the existing value; no need to tag the value pointer
     * since it is already tagged. */
    TABLE(anchor)[x_pos].as.kv.key = (void *)x_hash->key;
    TABLE(anchor)[x_pos].as.kv.value = x_value;
    /* fill in the new key/value pair, tagging the pointer to the
     * new value to mark it as a value ptr */
    TABLE(anchor)[pos].as.kv.key = key;
    TABLE(anchor)[pos].as.kv.value = tagged(value);

    return &TABLE(anchor)[pos];
}

static struct search_result
search_recursive(const struct hamt *h, struct hamt_node *anchor,
                 struct hash_state *hash, hamt_cmp_fn cmp_eq, const void *key,
                 struct hamt_node *path)
{
    assert(!is_value(VALUE(anchor)) &&
           "Invariant: path copy requires an internal node");
    struct hamt_node *copy = path;
    if (path) {
        /* copy the table we're pointing to */
        TABLE(copy) = table_dup(h, anchor);
        INDEX(copy) = INDEX(anchor);
        assert(!is_value(VALUE(copy)) && "Copy caused a leaf/internal switch");
    } else {
        copy = anchor;
    }

    /* determine the expected index in table */
    uint32_t expected_index = hash_get_index(hash);
    /* check if the expected index is set */
    if (has_index(copy, expected_index)) {
        /* if yes, get the compact index to address the array */
        int pos = get_pos(expected_index, INDEX(copy));
        /* index into the table and check what type of entry we're looking at */
        struct hamt_node *next = &TABLE(copy)[pos];
        if (is_value(VALUE(next))) {
            if ((*cmp_eq)(key, KEY(next)) == 0) {
                /* keys match */
                struct search_result result = {.status = SEARCH_SUCCESS,
                                               .anchor = copy,
                                               .value = next,
                                               .hash = hash};
                return result;
            }
            /* not found: same hash but different key */
            struct search_result result = {.status = SEARCH_FAIL_KEYMISMATCH,
                                           .anchor = copy,
                                           .value = next,
                                           .hash = hash};
            return result;
        } else {
            /* For table entries, recurse to the next level */
            assert(TABLE(next) != NULL &&
                   "invariant: table ptrs must not be NULL");
            return search_recursive(h, next, hash_next(hash), cmp_eq, key,
                                    path ? next : NULL);
        }
    }
    /* expected index is not set, terminate search */
    struct search_result result = {.status = SEARCH_FAIL_NOTFOUND,
                                   .anchor = copy,
                                   .value = NULL,
                                   .hash = hash};
    return result;
}

const void *hamt_get(const struct hamt *trie, void *key)
{
    struct hash_state *hash =
        &(struct hash_state){.key = key,
                             .hash_fn = trie->key_hash,
                             .hash = trie->key_hash(key, 0),
                             .depth = 0,
                             .shift = 0};
    struct search_result sr =
        search_recursive(trie, trie->root, hash, trie->key_cmp, key, NULL);
    if (sr.status == SEARCH_SUCCESS) {
        return untagged(sr.VALUE(value));
    }
    return NULL;
}

static const struct hamt_node *set(struct hamt *h, struct hamt_node *anchor,
                                   hamt_key_hash_fn hash_fn, hamt_cmp_fn cmp_fn,
                                   void *key, void *value)
{
    struct hash_state *hash = &(struct hash_state){.key = key,
                                                   .hash_fn = hash_fn,
                                                   .hash = hash_fn(key, 0),
                                                   .depth = 0,
                                                   .shift = 0};
    struct search_result sr =
        search_recursive(h, anchor, hash, cmp_fn, key, NULL);
    const struct hamt_node *inserted;
    switch (sr.status) {
    case SEARCH_SUCCESS:
        sr.VALUE(value) = tagged(value);
        inserted = sr.value;
        break;
    case SEARCH_FAIL_NOTFOUND:
        if ((inserted = insert_kv(h, sr.anchor, sr.hash, key, value)) != NULL) {
            h->size += 1;
        }
        break;
    case SEARCH_FAIL_KEYMISMATCH:
        if ((inserted = insert_table(h, sr.value, sr.hash, key, value)) !=
            NULL) {
            h->size += 1;
        }
        break;
    }
    return inserted;
}

const void *hamt_set(struct hamt *trie, void *key, void *value)
{
    const struct hamt_node *n =
        set(trie, trie->root, trie->key_hash, trie->key_cmp, key, value);
    return untagged(VALUE(n));
}

static struct path_result search(const struct hamt *h, struct hamt_node *anchor,
                                 struct hash_state *hash, hamt_cmp_fn cmp_eq,
                                 const void *key)
{
    struct path_result pr;
    pr.root = table_allocate(h, 1);
    pr.sr = search_recursive(h, anchor, hash, cmp_eq, key, pr.root);
    return pr;
}

const struct hamt *hamt_pset(const struct hamt *h, void *key, void *value)
{
    struct hash_state *hash = &(struct hash_state){.key = key,
                                                   .hash_fn = h->key_hash,
                                                   .hash = h->key_hash(key, 0),
                                                   .depth = 0,
                                                   .shift = 0};
    struct path_result pr = search(h, h->root, hash, h->key_cmp, key);
    struct hamt *cp = hamt_copy_shallow(h);
    cp->root = pr.root;
    switch (pr.sr.status) {
    case SEARCH_SUCCESS:
        pr.sr.VALUE(value) = tagged(value);
        break;
    case SEARCH_FAIL_NOTFOUND:
        if (insert_kv(cp, pr.sr.anchor, pr.sr.hash, key, value) != NULL) {
            cp->size += 1;
        }
        break;
    case SEARCH_FAIL_KEYMISMATCH:
        if (insert_table(cp, pr.sr.value, pr.sr.hash, key, value) != NULL) {
            cp->size += 1;
        }
        break;
    }
    return cp;
}

static struct remove_result
rem_recursive(struct hamt *h, struct hamt_node *root, struct hamt_node *anchor,
              struct hash_state *hash, hamt_cmp_fn cmp_eq, const void *key,
              struct hamt_node *path)
{
    assert(!is_value(VALUE(anchor)) &&
           "Invariant: removal requires an internal node");
    /* copy the table we're pointing to */
    struct hamt_node *copy = path;
    if (path) {
        TABLE(copy) = table_dup(h, anchor);
        INDEX(copy) = INDEX(anchor);
        assert(!is_value(VALUE(copy)) && "Copy caused a leaf/internal switch");
    } else {
        copy = anchor;
    }
    /* determine the expected index in table */
    uint32_t expected_index = hash_get_index(hash);
    /* check if the expected index is set */
    if (has_index(copy, expected_index)) {
        /* if yes, get the compact index to address the array */
        int pos = get_pos(expected_index, INDEX(copy));
        /* index into the table and check what type of entry we're looking at */
        struct hamt_node *next = &TABLE(copy)[pos];
        if (is_value(VALUE(next))) {
            if ((*cmp_eq)(key, KEY(next)) == 0) {
                uint32_t n_rows = get_popcount(INDEX(copy));
                void *value = VALUE(next);
                /* We shrink tables while they have more than 2 rows and switch
                 * to gathering the subtrie otherwise. The exception is when we
                 * are at the root, where we must shrink the table to one or
                 * zero.
                 */
                if (n_rows > 2 || (n_rows >= 1 && root == copy)) {
                    // FIXME: this sets copy to NULL when n_rows == 1
                    // i.e. when we remove the last entry from the trie
                    copy = table_shrink(h, copy, n_rows, expected_index, pos);
                } else if (n_rows == 2) {
                    /* if both rows are value rows, gather, dropping the current
                     * row */
                    struct hamt_node *other = &TABLE(copy)[!pos];
                    if (is_value(VALUE(other))) {
                        copy = table_gather(h, copy, !pos);
                        return (struct remove_result){.status = REMOVE_GATHERED,
                                                      .value = value};
                    } else {
                        /* otherwise shrink the node to n_rows == 1 */
                        copy =
                            table_shrink(h, copy, n_rows, expected_index, pos);
                    }
                }
                return (struct remove_result){.status = REMOVE_SUCCESS,
                                              .value = value};
            }
            /* not found: same hash but different key */
            return (struct remove_result){.status = REMOVE_NOTFOUND,
                                          .value = NULL};
        } else {
            /* For table entries, recurse to the next level */
            assert(TABLE(next) != NULL &&
                   "invariant: table ptrs must not be NULL");
            struct remove_result result =
                rem_recursive(h, root, next, hash_next(hash), cmp_eq, key,
                              path ? next : NULL);
            if (next != TABLE(root) && result.status == REMOVE_GATHERED) {
                /* remove dangling internal nodes: check if we need to
                 * propagate the gathering of the key-value entry */
                int n_rows = get_popcount(INDEX(copy));
                if (n_rows == 1) {
                    copy = table_gather(h, copy, 0);
                    return (struct remove_result){.status = REMOVE_GATHERED,
                                                  .value = result.value};
                }
            }
            return (struct remove_result){.status = REMOVE_SUCCESS,
                                          .value = result.value};
        }
    }
    return (struct remove_result){.status = REMOVE_NOTFOUND, .value = NULL};
}

static struct path_result rem(struct hamt *h, struct hamt_node *root,
                              struct hamt_node *anchor, struct hash_state *hash,
                              hamt_cmp_fn cmp_eq, const void *key)
{
    struct path_result pr;
    pr.root = table_allocate(h, 1);
    pr.rr = rem_recursive(h, pr.root, anchor, hash, cmp_eq, key, pr.root);
    return pr;
}

void *hamt_remove(struct hamt *trie, void *key)
{
    struct hash_state *hash =
        &(struct hash_state){.key = key,
                             .hash_fn = trie->key_hash,
                             .hash = trie->key_hash(key, 0),
                             .depth = 0,
                             .shift = 0};
    struct remove_result rr = rem_recursive(trie, trie->root, trie->root, hash,
                                            trie->key_cmp, key, NULL);
    if (rr.status == REMOVE_SUCCESS || rr.status == REMOVE_GATHERED) {
        trie->size -= 1;
        return untagged(rr.value);
    }
    return NULL;
}

const struct hamt *hamt_premove(const struct hamt *h, void *key)
{
    struct hash_state *hash =
        &(struct hash_state){.key = key,
                             .hash_fn = h->key_hash,
                             .hash = h->key_hash(key, 0),
                             .depth = 0,
                             .shift = 0};
    struct hamt *cp = hamt_copy_shallow(h);
    struct path_result pr = rem(cp, cp->root, cp->root, hash, cp->key_cmp, key);
    cp->root = pr.root;
    if (pr.rr.status == REMOVE_SUCCESS || pr.rr.status == REMOVE_GATHERED) {
        cp->size -= 1;
    }
    return cp;
}

/* delete recursively from anchor */
void delete_recursive(struct hamt *h, struct hamt_node *anchor)
{
    if (TABLE(anchor)) {
        assert(!is_value(VALUE(anchor)) && "delete requires an internal node");
        size_t n = get_popcount(INDEX(anchor));
        for (size_t i = 0; i < n; ++i) {
            if (!is_value(TABLE(anchor)[i].as.kv.value)) {
                delete_recursive(h, &TABLE(anchor)[i]);
            }
        }
        table_free(h, TABLE(anchor), n);
        TABLE(anchor) = NULL;
    }
}

void hamt_delete(struct hamt *h)
{
    delete_recursive(h, h->root);
#if defined(WITH_TABLE_CACHE)
    hamt_table_cache_destroy(h->table_ator, h->ator);
#endif
    mem_free(h->ator, h->root, sizeof(struct hamt_node));
    mem_free(h->ator, h, sizeof(struct hamt));
}

size_t hamt_size(const struct hamt *trie) { return trie->size; }

/** Iterators
 *
 * Iterators traverse the HAMT in DFS mode; each iterator instance maintains a
 * "call stack" of nodes encountered so far. Nodes are represented as a pair
 * of (a) a pointer to a `hamt_node` (aka an anchor) and a position that
 * indexes the current position in the table referred to by the anchor.
 *
 * The "call stack" is implemented with dynamic memory allocation to guard
 * against outliers in tree depth; that said, we expect the average stack
 * depth to be in the order of log(n) (where n is the number of items in the
 * HAMT)
 */

struct hamt_iterator_item {
    struct hamt_node *anchor;
    size_t pos;
    struct hamt_iterator_item *next;
};

struct hamt_iterator {
    struct hamt *trie;
    struct hamt_node *cur;
    struct hamt_iterator_item *tos; /* top of stack */
};

static struct hamt_iterator_item *iterator_push_item(struct hamt_iterator *it,
                                                     struct hamt_node *anchor,
                                                     size_t pos)
{
    /* push new item onto top of stack */
    struct hamt_iterator_item *new_item =
        mem_alloc(it->trie->ator, sizeof(struct hamt_iterator_item));
    if (new_item) {
        new_item->anchor = anchor;
        new_item->pos = pos;
        new_item->next = NULL;
        new_item->next = it->tos;
        it->tos = new_item;
    }
    return new_item;
}

static struct hamt_iterator_item *iterator_peek_item(struct hamt_iterator *it)
{
    return it->tos;
}

static struct hamt_iterator_item *iterator_pop_item(struct hamt_iterator *it)
{
    /* pop off top of stack */
    struct hamt_iterator_item *item = it->tos;
    if (item) {
        it->tos = it->tos->next;
    }
    return item;
}

struct hamt_iterator *hamt_it_create(struct hamt *trie)
{
    struct hamt_iterator *it =
        mem_alloc(trie->ator, sizeof(struct hamt_iterator));
    it->trie = trie;
    it->tos = NULL;
    it->cur = NULL;
    iterator_push_item(it, trie->root, 0);
    hamt_it_next(it);
    return it;
}

void hamt_it_delete(struct hamt_iterator *it)
{
    struct hamt_iterator_item *p = it->tos;
    struct hamt_iterator_item *tmp;
    while (p) {
        tmp = p;
        p = p->next;
        mem_free(it->trie->ator, tmp, sizeof(struct hamt_iterator_item));
    }
    mem_free(it->trie->ator, it, sizeof(struct hamt_iterator));
}

inline bool hamt_it_valid(struct hamt_iterator *it) { return it->cur != NULL; }

struct hamt_iterator *hamt_it_next(struct hamt_iterator *it)
{
    struct hamt_iterator_item *p;
    while (it && (p = iterator_peek_item(it)) != NULL) {
        /* determine number of entries / size of the table */
        int n_pos = get_popcount(INDEX(p->anchor));
        /* start from the table index we left off from */
        while (p->pos < n_pos) {
            /* get a pointer to the current subtrie */
            struct hamt_node *cur = &TABLE(p->anchor)[p->pos];
            /* increment the row count */
            p->pos++;
            /* check if we have a subtable or a value */
            if (is_value(VALUE(cur))) {
                /* cur refers to a value; set the iterator and bail */
                it->cur = cur;
                return it;
            } else {
                /* cur is a pointer to a subtable; push the item on
                 * the iterator stack and recurse */
                iterator_push_item(it, cur, 0);
                return hamt_it_next(it);
            }
        }
        /* remove table from stack when all rows have been dealt with */
        iterator_pop_item(it);
        mem_free(it->trie->ator, p, sizeof(struct hamt_iterator_item));
    }
    if (it)
        it->cur = NULL;
    return it;
}

const void *hamt_it_get_key(struct hamt_iterator *it)
{
    if (it->cur) {
        return KEY(it->cur);
    }
    return NULL;
}

const void *hamt_it_get_value(struct hamt_iterator *it)
{
    if (it->cur) {
        return untagged(VALUE(it->cur));
    }
    return NULL;
}
