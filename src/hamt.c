#include "hamt.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* #include "mem.h" */

/* Pointer tagging */
#define HAMT_TAG_MASK 0x3 /* last two bits */
#define HAMT_TAG_VALUE 0x1
#define tagged(__p) (HamtNode *)((uintptr_t)__p | HAMT_TAG_VALUE)
#define untagged(__p) (HamtNode *)((uintptr_t)__p & ~HAMT_TAG_MASK)
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
#define mem_alloc(ator, size) (ator)->malloc(size)
#define mem_realloc(ator, ptr, size) (ator)->realloc(ptr, size)
#define mem_free(ator, ptr) (ator)->free(ptr)
/* Default allocator uses system malloc */
struct HamtAllocator hamt_allocator_default = {malloc, realloc, free};

typedef struct HamtNode {
    union {
        struct {
            void *value; /* tagged pointer */
            void *key;
        } kv;
        struct {
            struct HamtNode *ptr;
            uint32_t index;
        } table;
    } as;
} HamtNode;

/* Opaque user-facing implementation */
struct HamtImpl {
    struct HamtNode *root;
    size_t size;
    HamtKeyHashFn key_hash;
    HamtCmpFn key_cmp;
    struct HamtAllocator *ator;
};

/* Hashing w/ state management */
typedef struct Hash {
    const void *key;
    HamtKeyHashFn hash_fn;
    uint32_t hash;
    size_t depth;
    size_t shift;
} Hash;

/* Search results */
typedef enum {
    SEARCH_SUCCESS,
    SEARCH_FAIL_NOTFOUND,
    SEARCH_FAIL_KEYMISMATCH
} SearchStatus;

typedef struct SearchResult {
    SearchStatus status;
    HamtNode *anchor;
    HamtNode *value;
    Hash *hash;
} SearchResult;

/* Removal results */
typedef enum { REMOVE_SUCCESS, REMOVE_GATHERED, REMOVE_NOTFOUND } RemoveStatus;

typedef struct RemoveResult {
    RemoveStatus status;
    void *value;
} RemoveResult;

typedef struct PathResult {
    union {
        SearchResult sr;
        RemoveResult rr;
    };
    HamtNode *root;
} PathResult;

static inline Hash *hash_next(Hash *h)
{
    h->depth += 1;
    h->shift += 5;
    if (h->shift > 30) {
        h->hash = h->hash_fn(h->key, h->depth / 5);
        h->shift = 0;
    }
    return h;
}

static inline uint32_t hash_get_index(const Hash *h)
{
    return (h->hash >> h->shift) & 0x1f;
}

static int get_popcount(uint32_t n) { return __builtin_popcount(n); }

static int get_pos(uint32_t sparse_index, uint32_t bitmap)
{
    return get_popcount(bitmap & ((1 << sparse_index) - 1));
}

static inline bool has_index(const HamtNode *anchor, size_t index)
{
    assert(anchor && "anchor must not be NULL");
    assert(index < 32 && "index must not be larger than 31");
    return INDEX(anchor) & (1 << index);
}

/* Table cache */
typedef struct FreeList
{
    size_t capacity;
    size_t size;
    HamtNode **ptrs;
} FreeList;

static bool freelist_initialized = false;
static FreeList freelist[32];

void table_freelist_print()
{
    printf("[");
    for (size_t i = 0; i < 31; ++i) {
        printf("%lu(%lu), ", freelist[i].size, freelist[i].capacity);
    }
    printf("%lu]\n", freelist[31].size);
}

void table_freelist_init(struct HamtAllocator *ator, size_t cachesize)
{
    static size_t freelist_bucket_capacities[32] = {
        4, 4, 4,
        8, 8,
        16, 16,
        32, 32,
        64, 64,
        128, 128, 128, 128,
        256, 256, 256, 256, 256, 256, 256, 256, 256,
        256, 256, 256, 256, 256, 256, 256, 256
    };
    /* cache size correction factor (since the reciprocals of the capacities do
     * not add up to 1 */
    double size_correction_factor = 0;
    double total_shares = 0;
    for (size_t i = 0; i < 32; ++i) {
        total_shares += 1.0 / freelist_bucket_capacities[i];
    }
    size_correction_factor = 1.0 / total_shares;

    for (size_t i = 0; i < 32; ++i) {
        freelist[i].capacity = (size_t) cachesize * size_correction_factor / freelist_bucket_capacities[i];
        freelist[i].size = 0;
        freelist[i].ptrs = mem_alloc(ator, freelist[i].capacity * sizeof(HamtNode*));
    }
    freelist_initialized = true;
    // table_freelist_print();
}


void table_freelist_destroy()
{
    // table_freelist_print();
    for (size_t i = 0; i < 32; ++i) {
        free(freelist[i].ptrs);
    }
    freelist_initialized = false;
}

void hamt_cache_init(struct HamtAllocator *ator, size_t cachesize)
{
    table_freelist_init(ator, cachesize);
}

void hamt_cache_destroy()
{
    table_freelist_destroy();
}

HamtNode *table_allocate(struct HamtAllocator *ator, size_t size)
{
    if (freelist_initialized) {
        size_t index = size - 1;
        if (size && freelist[index].size > 0) {
            size_t pos = freelist[index].size - 1;
            freelist[index].size--;
            return freelist[index].ptrs[pos];
        }
    }
    return (HamtNode *)mem_alloc(ator, (size * sizeof(HamtNode)));
}

void table_free(struct HamtAllocator *ator, HamtNode *ptr, size_t size)
{
    if (freelist_initialized) {
        size_t index = size - 1;
        if (size && ((freelist[index].size + 1) < freelist[index].capacity)) {
            size_t pos = freelist[index].size;
            freelist[index].ptrs[pos] = ptr;
            freelist[index].size++;
            return;
        }
    }
    mem_free(ator, ptr);
}

HamtNode *table_extend(struct HamtAllocator *ator, HamtNode *anchor,
                       size_t n_rows, uint32_t index, uint32_t pos)
{
    HamtNode *new_table = table_allocate(ator, n_rows + 1);
    if (!new_table)
        return NULL;
    if (n_rows > 0) {
        /* copy over table */
        memcpy(&new_table[0], &TABLE(anchor)[0], pos * sizeof(HamtNode));
        /* note: this works since (n_rows - pos) == 0 for cases
         * where we're adding the new k/v pair at the end (i.e. memcpy(a, b, 0)
         * is a nop) */
        memcpy(&new_table[pos + 1], &TABLE(anchor)[pos],
               (n_rows - pos) * sizeof(HamtNode));
    }
    assert(!is_value(VALUE(anchor)) && "URGS");
    table_free(ator, TABLE(anchor), n_rows);
    TABLE(anchor) = new_table;
    INDEX(anchor) |= (1 << index);
    return anchor;
}

HamtNode *table_shrink(struct HamtAllocator *ator, HamtNode *anchor,
                       size_t n_rows, uint32_t index, uint32_t pos)
{
    /* debug assertions */
    assert(anchor && "Anchor cannot be NULL");
    assert(!is_value(VALUE(anchor)) &&
           "Invariant: shrinking a table requires an internal node");

    HamtNode *new_table = NULL;
    uint32_t new_index = 0;
    if (n_rows > 0) {
        new_table = table_allocate(ator, n_rows - 1);
        if (!new_table)
            return NULL;
        new_index = INDEX(anchor) & ~(1 << index);
        memcpy(&new_table[0], &TABLE(anchor)[0], pos * sizeof(HamtNode));
        memcpy(&new_table[pos], &TABLE(anchor)[pos + 1],
               (n_rows - pos - 1) * sizeof(HamtNode));
    }
    table_free(ator, TABLE(anchor), n_rows);
    INDEX(anchor) = new_index;
    TABLE(anchor) = new_table;
    return anchor;
}

HamtNode *table_gather(struct HamtAllocator *ator, HamtNode *anchor,
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
    HamtNode *table = TABLE(anchor);
    KEY(anchor) = table[pos].as.kv.key;
    VALUE(anchor) = table[pos].as.kv.value; /* already tagged */
    table_free(ator, table, n_rows);
    return anchor;
}

HamtNode *table_dup(struct HamtAllocator *ator, HamtNode *anchor)
{
    int n_rows = get_popcount(INDEX(anchor));
    HamtNode *new_table = table_allocate(ator, n_rows);
    if (new_table) {
        memcpy(&new_table[0], &TABLE(anchor)[0], n_rows * sizeof(HamtNode));
    }
    return new_table;
}

HAMT hamt_create(HamtKeyHashFn key_hash, HamtCmpFn key_cmp,
                 struct HamtAllocator *ator)
{
    struct HamtImpl *trie = mem_alloc(ator, sizeof(struct HamtImpl));
    trie->ator = ator;
    trie->root = mem_alloc(ator, sizeof(HamtNode));
    memset(trie->root, 0, sizeof(HamtNode));
    trie->size = 0;
    trie->key_hash = key_hash;
    trie->key_cmp = key_cmp;
    return trie;
}

HAMT hamt_dup(HAMT h)
{
    struct HamtImpl *trie = mem_alloc(h->ator, sizeof(struct HamtImpl));
    trie->ator = h->ator;
    trie->root = mem_alloc(h->ator, sizeof(HamtNode));
    memcpy(trie->root, h->root, sizeof(HamtNode));
    trie->size = h->size;
    trie->key_hash = h->key_hash;
    trie->key_cmp = h->key_cmp;
    return trie;
}

static const HamtNode *insert_kv(HamtNode *anchor, Hash *hash, void *key,
                                 void *value, struct HamtAllocator *ator)
{
    /* calculate position in new table */
    uint32_t ix = hash_get_index(hash);
    uint32_t new_index = INDEX(anchor) | (1 << ix);
    int pos = get_pos(ix, new_index);
    /* extend table */
    size_t n_rows = get_popcount(INDEX(anchor));
    anchor = table_extend(ator, anchor, n_rows, ix, pos);
    if (!anchor)
        return NULL;
    HamtNode *new_table = TABLE(anchor);
    /* set new k/v pair */
    new_table[pos].as.kv.key = key;
    new_table[pos].as.kv.value = tagged(value);
    /* return a pointer to the inserted k/v pair */
    return &new_table[pos];
}

static const HamtNode *insert_table(HamtNode *anchor, Hash *hash, void *key,
                                    void *value, struct HamtAllocator *ator)
{
    /* FIXME: check for alloc failure and bail out correctly (deleting the
     *        incomplete subtree */

    /* Collect everything we know about the existing value */
    Hash *x_hash = &(Hash){.key = KEY(anchor),
                           .hash_fn = hash->hash_fn,
                           .hash = hash->hash_fn(KEY(anchor), hash->depth / 5),
                           .depth = hash->depth,
                           .shift = hash->shift};
    void *x_value = VALUE(anchor); /* tagged (!) value ptr */
    /* increase depth until the hashes diverge, building a list
     * of tables along the way */
    Hash *next_hash = hash_next(hash);
    Hash *x_next_hash = hash_next(x_hash);
    uint32_t next_index = hash_get_index(next_hash);
    uint32_t x_next_index = hash_get_index(x_next_hash);
    while (x_next_index == next_index) {
        TABLE(anchor) = table_allocate(ator, 1);
        INDEX(anchor) = (1 << next_index);
        next_hash = hash_next(next_hash);
        x_next_hash = hash_next(x_next_hash);
        next_index = hash_get_index(next_hash);
        x_next_index = hash_get_index(x_next_hash);
        anchor = TABLE(anchor);
    }
    /* the hashes are different, let's allocate a table with two
     * entries to store the existing and new values */
    TABLE(anchor) = table_allocate(ator, 2);
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

static SearchResult search_recursive(HamtNode *anchor, Hash *hash,
                                     HamtCmpFn cmp_eq, const void *key,
                                     HamtNode *path, struct HamtAllocator *ator)
{
    assert(!is_value(VALUE(anchor)) &&
           "Invariant: path copy requires an internal node");
    HamtNode *copy = path;
    if (path) {
        /* copy the table we're pointing to */
        TABLE(copy) = table_dup(ator, anchor);
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
        HamtNode *next = &TABLE(copy)[pos];
        if (is_value(VALUE(next))) {
            if ((*cmp_eq)(key, KEY(next)) == 0) {
                /* keys match */
                SearchResult result = {.status = SEARCH_SUCCESS,
                                       .anchor = copy,
                                       .value = next,
                                       .hash = hash};
                return result;
            }
            /* not found: same hash but different key */
            SearchResult result = {.status = SEARCH_FAIL_KEYMISMATCH,
                                   .anchor = copy,
                                   .value = next,
                                   .hash = hash};
            return result;
        } else {
            /* For table entries, recurse to the next level */
            assert(TABLE(next) != NULL &&
                   "invariant: table ptrs must not be NULL");
            return search_recursive(next, hash_next(hash), cmp_eq, key,
                                    path ? next : NULL, ator);
        }
    }
    /* expected index is not set, terminate search */
    SearchResult result = {.status = SEARCH_FAIL_NOTFOUND,
                           .anchor = copy,
                           .value = NULL,
                           .hash = hash};
    return result;
}

const void *hamt_get(const HAMT trie, void *key)
{
    Hash *hash = &(Hash){.key = key,
                         .hash_fn = trie->key_hash,
                         .hash = trie->key_hash(key, 0),
                         .depth = 0,
                         .shift = 0};
    SearchResult sr = search_recursive(trie->root, hash, trie->key_cmp, key,
                                       NULL, trie->ator);
    if (sr.status == SEARCH_SUCCESS) {
        return untagged(sr.VALUE(value));
    }
    return NULL;
}

static const HamtNode *set(HAMT h, HamtNode *anchor, HamtKeyHashFn hash_fn,
                           HamtCmpFn cmp_fn, void *key, void *value)
{
    Hash *hash = &(Hash){.key = key,
                         .hash_fn = hash_fn,
                         .hash = hash_fn(key, 0),
                         .depth = 0,
                         .shift = 0};
    SearchResult sr =
        search_recursive(anchor, hash, cmp_fn, key, NULL, h->ator);
    const HamtNode *inserted;
    switch (sr.status) {
    case SEARCH_SUCCESS:
        sr.VALUE(value) = tagged(value);
        inserted = sr.value;
        break;
    case SEARCH_FAIL_NOTFOUND:
        if ((inserted = insert_kv(sr.anchor, sr.hash, key, value, h->ator)) !=
            NULL) {
            h->size += 1;
        }
        break;
    case SEARCH_FAIL_KEYMISMATCH:
        if ((inserted = insert_table(sr.value, sr.hash, key, value, h->ator)) !=
            NULL) {
            h->size += 1;
        }
        break;
    }
    return inserted;
}

const void *hamt_set(HAMT trie, void *key, void *value)
{
    const HamtNode *n =
        set(trie, trie->root, trie->key_hash, trie->key_cmp, key, value);
    return VALUE(n);
}

static PathResult search(HamtNode *anchor, Hash *hash, HamtCmpFn cmp_eq,
                         const void *key, struct HamtAllocator *ator)
{
    PathResult pr;
    pr.root = mem_alloc(ator, sizeof(HamtNode));
    pr.sr = search_recursive(anchor, hash, cmp_eq, key, pr.root, ator);
    return pr;
}

const HAMT hamt_pset(HAMT h, void *key, void *value)
{
    Hash *hash = &(Hash){.key = key,
                         .hash_fn = h->key_hash,
                         .hash = h->key_hash(key, 0),
                         .depth = 0,
                         .shift = 0};
    HAMT cp = hamt_dup(h);
    PathResult pr = search(h->root, hash, h->key_cmp, key, h->ator);
    cp->root = pr.root;
    switch (pr.sr.status) {
    case SEARCH_SUCCESS:
        pr.sr.VALUE(value) = tagged(value);
        break;
    case SEARCH_FAIL_NOTFOUND:
        if (insert_kv(pr.sr.anchor, pr.sr.hash, key, value, h->ator) != NULL) {
            cp->size += 1;
        }
        break;
    case SEARCH_FAIL_KEYMISMATCH:
        if (insert_table(pr.sr.value, pr.sr.hash, key, value, h->ator) !=
            NULL) {
            cp->size += 1;
        }
        break;
    }
    return cp;
}

static RemoveResult rem_recursive(HamtNode *root, HamtNode *anchor, Hash *hash,
                                  HamtCmpFn cmp_eq, const void *key,
                                  HamtNode *path, struct HamtAllocator *ator)
{
    assert(!is_value(VALUE(anchor)) &&
           "Invariant: removal requires an internal node");
    /* copy the table we're pointing to */
    HamtNode *copy = path;
    if (path) {
        TABLE(copy) = table_dup(ator, anchor);
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
        HamtNode *next = &TABLE(copy)[pos];
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
                    copy =
                        table_shrink(ator, copy, n_rows, expected_index, pos);
                } else if (n_rows == 2) {
                    /* if both rows are value rows, gather, dropping the current
                     * row */
                    HamtNode *other = &TABLE(copy)[!pos];
                    if (is_value(VALUE(other))) {
                        copy = table_gather(ator, copy, !pos);
                        return (RemoveResult){.status = REMOVE_GATHERED,
                                              .value = value};
                    } else {
                        /* otherwise shrink the node to n_rows == 1 */
                        copy = table_shrink(ator, copy, n_rows, expected_index,
                                            pos);
                    }
                }
                return (RemoveResult){.status = REMOVE_SUCCESS, .value = value};
            }
            /* not found: same hash but different key */
            return (RemoveResult){.status = REMOVE_NOTFOUND, .value = NULL};
        } else {
            /* For table entries, recurse to the next level */
            assert(TABLE(next) != NULL &&
                   "invariant: table ptrs must not be NULL");
            RemoveResult result =
                rem_recursive(root, next, hash_next(hash), cmp_eq, key,
                              path ? next : NULL, ator);
            if (next != TABLE(root) && result.status == REMOVE_GATHERED) {
                /* remove dangling internal nodes: check if we need to
                 * propagate the gathering of the key-value entry */
                int n_rows = get_popcount(INDEX(copy));
                if (n_rows == 1) {
                    copy = table_gather(ator, copy, 0);
                    return (RemoveResult){.status = REMOVE_GATHERED,
                                          .value = result.value};
                }
            }
            return (RemoveResult){.status = REMOVE_SUCCESS,
                                  .value = result.value};
        }
    }
    return (RemoveResult){.status = REMOVE_NOTFOUND, .value = NULL};
}

static PathResult rem(HamtNode *root, HamtNode *anchor, Hash *hash,
                      HamtCmpFn cmp_eq, const void *key,
                      struct HamtAllocator *ator)
{
    PathResult pr;
    pr.root = mem_alloc(ator, sizeof(HamtNode));
    pr.rr = rem_recursive(pr.root, anchor, hash, cmp_eq, key, pr.root, ator);
    return pr;
}

void *hamt_remove(HAMT trie, void *key)
{
    Hash *hash = &(Hash){.key = key,
                         .hash_fn = trie->key_hash,
                         .hash = trie->key_hash(key, 0),
                         .depth = 0,
                         .shift = 0};
    RemoveResult rr = rem_recursive(trie->root, trie->root, hash, trie->key_cmp,
                                    key, NULL, trie->ator);
    if (rr.status == REMOVE_SUCCESS || rr.status == REMOVE_GATHERED) {
        trie->size -= 1;
        return untagged(rr.value);
    }
    return NULL;
}

const HAMT hamt_premove(const HAMT trie, void *key)
{
    Hash *hash = &(Hash){.key = key,
                         .hash_fn = trie->key_hash,
                         .hash = trie->key_hash(key, 0),
                         .depth = 0,
                         .shift = 0};
    HAMT cp = hamt_dup(trie);
    PathResult pr =
        rem(trie->root, trie->root, hash, trie->key_cmp, key, trie->ator);
    cp->root = pr.root;
    if (pr.rr.status == REMOVE_SUCCESS || pr.rr.status == REMOVE_GATHERED) {
        cp->size -= 1;
    }
    return cp;
}
void delete (HamtNode *anchor, struct HamtAllocator *ator)
{
    if (TABLE(anchor)) {
        assert(!is_value(VALUE(anchor)) && "delete requires an internal node");
        size_t n = get_popcount(INDEX(anchor));
        for (size_t i = 0; i < n; ++i) {
            if (!is_value(TABLE(anchor)[i].as.kv.value)) {
                delete (&TABLE(anchor)[i], ator);
            }
        }
        table_free(ator, TABLE(anchor), n);
        TABLE(anchor) = NULL;
    }
}

void hamt_delete(HAMT h)
{
    delete (h->root, h->ator);
    mem_free(h->ator, h->root);
    mem_free(h->ator, h);
}

size_t hamt_size(const HAMT trie) { return trie->size; }

struct HamtIteratorItem {
    HamtNode *anchor;
    size_t pos;
    struct HamtIteratorItem *next;
};

struct HamtIteratorImpl {
    HAMT trie;
    HamtNode *cur;
    struct HamtIteratorItem *head, *tail;
};

static struct HamtIteratorItem *iterator_push_item(HamtIterator it,
                                                   HamtNode *anchor, size_t pos)
{
    /* append at the end */
    struct HamtIteratorItem *new_item =
        mem_alloc(it->trie->ator, sizeof(struct HamtIteratorItem));
    if (new_item) {
        new_item->anchor = anchor;
        new_item->pos = pos;
        new_item->next = NULL;
        if (it->tail) {
            it->tail->next = new_item;
        } else {
            /* first insert */
            it->tail = it->head = new_item;
        }
    }
    return new_item;
}

static struct HamtIteratorItem *iterator_peek_item(HamtIterator it)
{
    return it->head;
}

static struct HamtIteratorItem *iterator_pop_item(HamtIterator it)
{
    /* pop from front */
    struct HamtIteratorItem *top = it->head;
    it->head = it->head->next;
    return top;
}

HamtIterator hamt_it_create(const HAMT trie)
{
    struct HamtIteratorImpl *it =
        mem_alloc(trie->ator, sizeof(struct HamtIteratorImpl));
    it->trie = trie;
    it->cur = NULL;
    it->head = it->tail = NULL;
    iterator_push_item(it, trie->root, 0);
    it->head = it->tail;
    hamt_it_next(it);
    return it;
}

void hamt_it_delete(HamtIterator it)
{
    struct HamtIteratorItem *p = it->head;
    struct HamtIteratorItem *tmp;
    while (p) {
        tmp = p;
        p = p->next;
        mem_free(it->trie->ator, tmp);
    }
    mem_free(it->trie->ator, it);
}

inline bool hamt_it_valid(HamtIterator it) { return it->cur != NULL; }

HamtIterator hamt_it_next(HamtIterator it)
{
    struct HamtIteratorItem *p;
    while (it && (p = iterator_peek_item(it)) != NULL) {
        int n_rows = get_popcount(INDEX(p->anchor));
        for (int i = p->pos; i < n_rows; ++i) {
            HamtNode *cur = &TABLE(p->anchor)[i];
            if (is_value(VALUE(cur))) {
                if (i < n_rows - 1) {
                    p->pos = i + 1;
                } else {
                    iterator_pop_item(it);
                }
                it->cur = cur;
                return it;
            }
            /* cur is a pointer to a subtable */
            iterator_push_item(it, cur, 0);
        }
        iterator_pop_item(it);
    }
    it->cur = NULL;
    return it;
}

const void *hamt_it_get_key(HamtIterator it)
{
    if (it->cur) {
        return KEY(it->cur);
    }
    return NULL;
}

const void *hamt_it_get_value(HamtIterator it)
{
    if (it->cur) {
        return untagged(VALUE(it->cur));
    }
    return NULL;
}
