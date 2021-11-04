#include "hamt.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mem.h"

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

HAMT hamt_create(HamtKeyHashFn key_hash, HamtCmpFn key_cmp)
{
    struct HamtImpl *trie = mem_alloc(sizeof(struct HamtImpl));
    trie->root = mem_alloc(sizeof(HamtNode));
    memset(trie->root, 0, sizeof(HamtNode));
    trie->size = 0;
    trie->key_hash = key_hash;
    trie->key_cmp = key_cmp;
    return trie;
}

static SearchResult search(HamtNode *anchor, Hash *hash, HamtCmpFn cmp_eq,
                           const void *key)
{
    assert(!is_value(VALUE(anchor)) &&
           "Invariant: search requires an internal node");
    /* determine the expected index in table */
    uint32_t expected_index = hash_get_index(hash);
    /* check if the expected index is set */
    if (has_index(anchor, expected_index)) {
        /* if yes, get the compact index to address the array */
        int pos = get_pos(expected_index, INDEX(anchor));
        /* index into the table and check what type of entry we're looking at */
        HamtNode *next = &TABLE(anchor)[pos];
        if (is_value(VALUE(next))) {
            if ((*cmp_eq)(key, KEY(next)) == 0) {
                /* keys match */
                SearchResult result = {.status = SEARCH_SUCCESS,
                                       .anchor = anchor,
                                       .value = next,
                                       .hash = hash};
                return result;
            }
            /* not found: same hash but different key */
            SearchResult result = {.status = SEARCH_FAIL_KEYMISMATCH,
                                   .anchor = anchor,
                                   .value = next,
                                   .hash = hash};
            return result;
        } else {
            /* For table entries, recurse to the next level */
            assert(TABLE(next) != NULL &&
                   "invariant: table ptrs must not be NULL");
            return search(next, hash_next(hash), cmp_eq, key);
        }
    }
    /* expected index is not set, terminate search */
    SearchResult result = {.status = SEARCH_FAIL_NOTFOUND,
                           .anchor = anchor,
                           .value = NULL,
                           .hash = hash};
    return result;
}

HamtNode *mem_allocate_table(size_t size)
{
    return (HamtNode *)mem_alloc(size * sizeof(HamtNode));
}

void mem_free_table(HamtNode *ptr, size_t size) { mem_free(ptr); }

HamtNode *mem_extend_table(HamtNode *anchor, size_t n_rows, uint32_t index,
                           uint32_t pos)
{
    HamtNode *new_table = mem_allocate_table(n_rows + 1);
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
    mem_free_table(TABLE(anchor), n_rows);
    TABLE(anchor) = new_table;
    INDEX(anchor) |= (1 << index);
    return anchor;
}

HamtNode *mem_shrink_table(HamtNode *anchor, size_t n_rows, uint32_t index,
                           uint32_t pos)
{
    /* debug assertions */
    assert(anchor && "Anchor cannot be NULL");
    assert(!is_value(VALUE(anchor)) &&
           "Invariant: shrinking a table requires an internal node");

    HamtNode *new_table = NULL;
    uint32_t new_index = 0;
    if (n_rows > 0) {
        new_table = mem_allocate_table(n_rows - 1);
        if (!new_table)
            return NULL;
        new_index = INDEX(anchor) & ~(1 << index);
        memcpy(&new_table[0], &TABLE(anchor)[0], pos * sizeof(HamtNode));
        memcpy(&new_table[pos], &TABLE(anchor)[pos + 1],
               (n_rows - pos - 1) * sizeof(HamtNode));
    }
    mem_free_table(TABLE(anchor), n_rows);
    INDEX(anchor) = new_index;
    TABLE(anchor) = new_table;
    return anchor;
}

HamtNode *mem_gather_table(HamtNode *anchor, uint32_t pos)
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
    mem_free_table(table, n_rows);
    return anchor;
}

static const HamtNode *insert_kv(HamtNode *anchor, Hash *hash, void *key,
                                 void *value)
{
    /* calculate position in new table */
    uint32_t ix = hash_get_index(hash);
    uint32_t new_index = INDEX(anchor) | (1 << ix);
    int pos = get_pos(ix, new_index);
    /* extend table */
    size_t n_rows = get_popcount(INDEX(anchor));
    anchor = mem_extend_table(anchor, n_rows, ix, pos);
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
                                    void *value)
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
        TABLE(anchor) = mem_allocate_table(1);
        INDEX(anchor) = (1 << next_index);
        next_hash = hash_next(next_hash);
        x_next_hash = hash_next(x_next_hash);
        next_index = hash_get_index(next_hash);
        x_next_index = hash_get_index(x_next_hash);
        anchor = TABLE(anchor);
    }
    /* the hashes are different, let's allocate a table with two
     * entries to store the existing and new values */
    TABLE(anchor) = mem_allocate_table(2);
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

static const HamtNode *set(HAMT h, HamtNode *anchor, HamtKeyHashFn hash_fn,
                           HamtCmpFn cmp_fn, void *key, void *value)
{
    Hash *hash = &(Hash){.key = key,
                         .hash_fn = hash_fn,
                         .hash = hash_fn(key, 0),
                         .depth = 0,
                         .shift = 0};
    SearchResult sr = search(anchor, hash, cmp_fn, key);
    const HamtNode *inserted;
    switch (sr.status) {
    case SEARCH_SUCCESS:
        sr.VALUE(value) = tagged(value);
        inserted = sr.value;
        break;
    case SEARCH_FAIL_NOTFOUND:
        if ((inserted = insert_kv(sr.anchor, sr.hash, key, value)) != NULL) {
            h->size += 1;
        }
        break;
    case SEARCH_FAIL_KEYMISMATCH:
        if ((inserted = insert_table(sr.value, sr.hash, key, value)) != NULL) {
            h->size += 1;
        }
        break;
    }
    return inserted;
}

const void *hamt_get(const HAMT trie, void *key)
{
    Hash *hash = &(Hash){.key = key,
                         .hash_fn = trie->key_hash,
                         .hash = trie->key_hash(key, 0),
                         .depth = 0,
                         .shift = 0};
    SearchResult sr = search(trie->root, hash, trie->key_cmp, key);
    if (sr.status == SEARCH_SUCCESS) {
        return untagged(sr.VALUE(value));
    }
    return NULL;
}

const void *hamt_set(HAMT trie, void *key, void *value)
{
    const HamtNode *n =
        set(trie, trie->root, trie->key_hash, trie->key_cmp, key, value);
    return VALUE(n);
}

void delete (HamtNode *anchor)
{
    if (TABLE(anchor)) {
        assert(!is_value(VALUE(anchor)) && "delete requires an internal node");
        size_t n = get_popcount(INDEX(anchor));
        for (size_t i = 0; i < n; ++i) {
            if (!is_value(TABLE(anchor)[i].as.kv.value)) {
                delete (&TABLE(anchor)[i]);
            }
        }
        mem_free_table(TABLE(anchor), n);
        TABLE(anchor) = NULL;
    }
}

void hamt_delete(HAMT trie)
{
    delete (trie->root);
    mem_free(trie->root);
    mem_free(trie);
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
    struct HamtIteratorItem *new_item = malloc(sizeof(struct HamtIteratorItem));
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
    struct HamtIteratorImpl *it = mem_alloc(sizeof(struct HamtIteratorImpl));
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
        mem_free(tmp);
    }
    mem_free(it);
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

/*
 * ============    persistent data strcuture code    ===========
 */
HAMT hamt_dup(HAMT h)
{
    struct HamtImpl *trie = mem_alloc(sizeof(struct HamtImpl));
    trie->root = mem_alloc(sizeof(HamtNode));
    memcpy(trie->root, h->root, sizeof(HamtNode));
    trie->size = h->size;
    trie->key_hash = h->key_hash;
    trie->key_cmp = h->key_cmp;
    return trie;
}

HamtNode *mem_dup_table(HamtNode *anchor)
{
    int n_rows = get_popcount(INDEX(anchor));
    HamtNode *new_table = mem_allocate_table(n_rows);
    if (new_table) {
        memcpy(&new_table[0], &TABLE(anchor)[0], n_rows * sizeof(HamtNode));
    }
    return new_table;
}

static SearchResult path_copy_search_recurse(HamtNode *anchor, Hash *hash,
                                             HamtCmpFn cmp_eq, const void *key,
                                             HamtNode *copy)
{
    assert(!is_value(VALUE(anchor)) &&
           "Invariant: path copy requires an internal node");
    /* copy the table we're pointing to */
    TABLE(copy) = mem_dup_table(anchor);
    INDEX(copy) = INDEX(anchor);
    assert(!is_value(VALUE(copy)) && "Copy caused a leaf/internal switch");

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
            return path_copy_search_recurse(next, hash_next(hash), cmp_eq, key,
                                            next);
        }
    }
    /* expected index is not set, terminate search */
    SearchResult result = {.status = SEARCH_FAIL_NOTFOUND,
                           .anchor = copy,
                           .value = NULL,
                           .hash = hash};
    return result;
}

static PathResult path_copy_search(HamtNode *anchor, Hash *hash,
                                   HamtCmpFn cmp_eq, const void *key)
{
    PathResult pr;
    pr.root = mem_alloc(sizeof(HamtNode));
    pr.sr = path_copy_search_recurse(anchor, hash, cmp_eq, key, pr.root);
    return pr;
}

static const HAMT pset(HAMT h, HamtNode *anchor, HamtKeyHashFn hash_fn,
                       HamtCmpFn cmp_fn, void *key, void *value)
{
    Hash *hash = &(Hash){.key = key,
                         .hash_fn = hash_fn,
                         .hash = hash_fn(key, 0),
                         .depth = 0,
                         .shift = 0};
    HAMT cp = hamt_dup(h);
    PathResult pr = path_copy_search(anchor, hash, cmp_fn, key);
    cp->root = pr.root;
    switch (pr.sr.status) {
    case SEARCH_SUCCESS:
        pr.sr.VALUE(value) = tagged(value);
        break;
    case SEARCH_FAIL_NOTFOUND:
        if (insert_kv(pr.sr.anchor, pr.sr.hash, key, value) != NULL) {
            cp->size += 1;
        }
        break;
    case SEARCH_FAIL_KEYMISMATCH:
        if (insert_table(pr.sr.value, pr.sr.hash, key, value) != NULL) {
            cp->size += 1;
        }
        break;
    }
    return cp;
}

const HAMT hamt_pset(HAMT trie, void *key, void *value)
{
    return pset(trie, trie->root, trie->key_hash, trie->key_cmp, key, value);
}

static RemoveResult path_copy_rem_recurse(HamtNode *root, HamtNode *anchor,
                                          Hash *hash, HamtCmpFn cmp_eq,
                                          const void *key, HamtNode *path)
{
    assert(!is_value(VALUE(anchor)) &&
           "Invariant: removal requires an internal node");
    /* copy the table we're pointing to */
    HamtNode *copy = path;
    if (copy) {
        TABLE(copy) = mem_dup_table(anchor);
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
                    copy = mem_shrink_table(copy, n_rows, expected_index, pos);
                } else if (n_rows == 2) {
                    /* if both rows are value rows, gather, dropping the current
                     * row */
                    HamtNode *other = &TABLE(copy)[!pos];
                    if (is_value(VALUE(other))) {
                        copy = mem_gather_table(copy, !pos);
                        return (RemoveResult){.status = REMOVE_GATHERED,
                                              .value = value};
                    } else {
                        /* otherwise shrink the node to n_rows == 1 */
                        copy =
                            mem_shrink_table(copy, n_rows, expected_index, pos);
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
            RemoveResult result = path_copy_rem_recurse(
                root, next, hash_next(hash), cmp_eq, key, next);
            if (next != TABLE(root) && result.status == REMOVE_GATHERED) {
                /* remove dangling internal nodes: check if we need to
                 * propagate the gathering of the key-value entry */
                int n_rows = get_popcount(INDEX(copy));
                if (n_rows == 1) {
                    copy = mem_gather_table(copy, 0);
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

static PathResult path_copy_rem(HamtNode *root, HamtNode *anchor, Hash *hash,
                                HamtCmpFn cmp_eq, const void *key)
{
    PathResult pr;
    pr.root = mem_alloc(sizeof(HamtNode));
    pr.rr = path_copy_rem_recurse(pr.root, anchor, hash, cmp_eq, key, pr.root);
    return pr;
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
        path_copy_rem(trie->root, trie->root, hash, trie->key_cmp, key);
    cp->root = pr.root;
    if (pr.rr.status == REMOVE_SUCCESS || pr.rr.status == REMOVE_GATHERED) {
        cp->size -= 1;
    }
    return cp;
}

void *hamt_remove(HAMT trie, void *key)
{
    Hash *hash = &(Hash){.key = key,
                         .hash_fn = trie->key_hash,
                         .hash = trie->key_hash(key, 0),
                         .depth = 0,
                         .shift = 0};
    RemoveResult rr = path_copy_rem_recurse(trie->root, trie->root, hash, trie->key_cmp, key, NULL);
    if (rr.status == REMOVE_SUCCESS || rr.status == REMOVE_GATHERED) {
        trie->size -= 1;
        return untagged(rr.value);
    }
    return NULL;
}

