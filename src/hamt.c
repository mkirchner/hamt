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
#define tagged(__p) (hamt_node *)((uintptr_t)__p | HAMT_TAG_VALUE)
#define untagged(__p) (hamt_node *)((uintptr_t)__p & ~HAMT_TAG_MASK)
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
struct hamt_allocator hamt_allocator_default = {malloc, realloc, free};

typedef struct hamt_node {
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
} hamt_node;

/* Opaque user-facing implementation */
struct hamt_impl {
    struct hamt_node *root;
    size_t size;
    hamt_key_hash_fn key_hash;
    hamt_cmp_fn key_cmp;
    struct hamt_allocator *ator;
};

/* hash_stateing w/ state management */
typedef struct hash_state {
    const void *key;
    hamt_key_hash_fn hash_fn;
    uint32_t hash;
    size_t depth;
    size_t shift;
} hash_state;

/* Search results */
typedef enum {
    SEARCH_SUCCESS,
    SEARCH_FAIL_NOTFOUND,
    SEARCH_FAIL_KEYMISMATCH
} search_status;

typedef struct search_result {
    search_status status;
    hamt_node *anchor;
    hamt_node *value;
    hash_state *hash;
} search_result;

/* Removal results */
typedef enum { REMOVE_SUCCESS, REMOVE_GATHERED, REMOVE_NOTFOUND } remove_status;

typedef struct remove_result {
    remove_status status;
    void *value;
} remove_result;

typedef struct path_result {
    union {
        search_result sr;
        remove_result rr;
    };
    hamt_node *root;
} path_result;

static inline hash_state *hash_next(hash_state *h)
{
    h->depth += 1;
    h->shift += 5;
    if (h->shift > 30) {
        h->hash = h->hash_fn(h->key, h->depth / 5);
        h->shift = 0;
    }
    return h;
}

static inline uint32_t hash_get_index(const hash_state *h)
{
    return (h->hash >> h->shift) & 0x1f;
}

static int get_popcount(uint32_t n) { return __builtin_popcount(n); }

static int get_pos(uint32_t sparse_index, uint32_t bitmap)
{
    return get_popcount(bitmap & ((1 << sparse_index) - 1));
}

static inline bool has_index(const hamt_node *anchor, size_t index)
{
    assert(anchor && "anchor must not be NULL");
    assert(index < 32 && "index must not be larger than 31");
    return INDEX(anchor) & (1 << index);
}

hamt_node *table_allocate(struct hamt_allocator *ator, size_t size)
{
    return (hamt_node *)mem_alloc(ator, (size * sizeof(hamt_node)));
}

void table_free(struct hamt_allocator *ator, hamt_node *ptr, size_t size)
{
    mem_free(ator, ptr);
}

hamt_node *table_extend(struct hamt_allocator *ator, hamt_node *anchor,
                        size_t n_rows, uint32_t index, uint32_t pos)
{
    hamt_node *new_table = table_allocate(ator, n_rows + 1);
    if (!new_table)
        return NULL;
    if (n_rows > 0) {
        /* copy over table */
        memcpy(&new_table[0], &TABLE(anchor)[0], pos * sizeof(hamt_node));
        /* note: this works since (n_rows - pos) == 0 for cases
         * where we're adding the new k/v pair at the end (i.e. memcpy(a, b, 0)
         * is a nop) */
        memcpy(&new_table[pos + 1], &TABLE(anchor)[pos],
               (n_rows - pos) * sizeof(hamt_node));
    }
    assert(!is_value(VALUE(anchor)) && "URGS");
    table_free(ator, TABLE(anchor), n_rows);
    TABLE(anchor) = new_table;
    INDEX(anchor) |= (1 << index);
    return anchor;
}

hamt_node *table_shrink(struct hamt_allocator *ator, hamt_node *anchor,
                        size_t n_rows, uint32_t index, uint32_t pos)
{
    /* debug assertions */
    assert(anchor && "Anchor cannot be NULL");
    assert(!is_value(VALUE(anchor)) &&
           "Invariant: shrinking a table requires an internal node");

    hamt_node *new_table = NULL;
    uint32_t new_index = 0;
    if (n_rows > 0) {
        new_table = table_allocate(ator, n_rows - 1);
        if (!new_table)
            return NULL;
        new_index = INDEX(anchor) & ~(1 << index);
        memcpy(&new_table[0], &TABLE(anchor)[0], pos * sizeof(hamt_node));
        memcpy(&new_table[pos], &TABLE(anchor)[pos + 1],
               (n_rows - pos - 1) * sizeof(hamt_node));
    }
    table_free(ator, TABLE(anchor), n_rows);
    INDEX(anchor) = new_index;
    TABLE(anchor) = new_table;
    return anchor;
}

hamt_node *table_gather(struct hamt_allocator *ator, hamt_node *anchor,
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
    hamt_node *table = TABLE(anchor);
    KEY(anchor) = table[pos].as.kv.key;
    VALUE(anchor) = table[pos].as.kv.value; /* already tagged */
    table_free(ator, table, n_rows);
    return anchor;
}

hamt_node *table_dup(struct hamt_allocator *ator, hamt_node *anchor)
{
    int n_rows = get_popcount(INDEX(anchor));
    hamt_node *new_table = table_allocate(ator, n_rows);
    if (new_table) {
        memcpy(&new_table[0], &TABLE(anchor)[0], n_rows * sizeof(hamt_node));
    }
    return new_table;
}

HAMT hamt_create(hamt_key_hash_fn key_hash, hamt_cmp_fn key_cmp,
                 struct hamt_allocator *ator)
{
    struct hamt_impl *trie = mem_alloc(ator, sizeof(struct hamt_impl));
    trie->ator = ator;
    trie->root = mem_alloc(ator, sizeof(hamt_node));
    memset(trie->root, 0, sizeof(hamt_node));
    trie->size = 0;
    trie->key_hash = key_hash;
    trie->key_cmp = key_cmp;
    return trie;
}

HAMT hamt_dup(HAMT h)
{
    struct hamt_impl *trie = mem_alloc(h->ator, sizeof(struct hamt_impl));
    trie->ator = h->ator;
    trie->root = mem_alloc(h->ator, sizeof(hamt_node));
    memcpy(trie->root, h->root, sizeof(hamt_node));
    trie->size = h->size;
    trie->key_hash = h->key_hash;
    trie->key_cmp = h->key_cmp;
    return trie;
}

static const hamt_node *insert_kv(hamt_node *anchor, hash_state *hash,
                                  void *key, void *value,
                                  struct hamt_allocator *ator)
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
    hamt_node *new_table = TABLE(anchor);
    /* set new k/v pair */
    new_table[pos].as.kv.key = key;
    new_table[pos].as.kv.value = tagged(value);
    /* return a pointer to the inserted k/v pair */
    return &new_table[pos];
}

static const hamt_node *insert_table(hamt_node *anchor, hash_state *hash,
                                     void *key, void *value,
                                     struct hamt_allocator *ator)
{
    /* FIXME: check for alloc failure and bail out correctly (deleting the
     *        incomplete subtree */

    /* Collect everything we know about the existing value */
    hash_state *x_hash =
        &(hash_state){.key = KEY(anchor),
                      .hash_fn = hash->hash_fn,
                      .hash = hash->hash_fn(KEY(anchor), hash->depth / 5),
                      .depth = hash->depth,
                      .shift = hash->shift};
    void *x_value = VALUE(anchor); /* tagged (!) value ptr */
    /* increase depth until the hashes diverge, building a list
     * of tables along the way */
    hash_state *next_hash = hash_next(hash);
    hash_state *x_next_hash = hash_next(x_hash);
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

static search_result search_recursive(hamt_node *anchor, hash_state *hash,
                                      hamt_cmp_fn cmp_eq, const void *key,
                                      hamt_node *path,
                                      struct hamt_allocator *ator)
{
    assert(!is_value(VALUE(anchor)) &&
           "Invariant: path copy requires an internal node");
    hamt_node *copy = path;
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
        hamt_node *next = &TABLE(copy)[pos];
        if (is_value(VALUE(next))) {
            if ((*cmp_eq)(key, KEY(next)) == 0) {
                /* keys match */
                search_result result = {.status = SEARCH_SUCCESS,
                                        .anchor = copy,
                                        .value = next,
                                        .hash = hash};
                return result;
            }
            /* not found: same hash but different key */
            search_result result = {.status = SEARCH_FAIL_KEYMISMATCH,
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
    search_result result = {.status = SEARCH_FAIL_NOTFOUND,
                            .anchor = copy,
                            .value = NULL,
                            .hash = hash};
    return result;
}

static search_result search_iterative(hamt_node *anchor, hash_state *hash,
                                      hamt_cmp_fn cmp_eq, const void *key,
                                      hamt_node *path,
                                      struct hamt_allocator *ator)
{
    while (1) {
        assert(!is_value(VALUE(anchor)) &&
               "Invariant: path copy requires an internal node");
        hamt_node *copy = path;
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
            hamt_node *next = &TABLE(copy)[pos];
            if (is_value(VALUE(next))) {
                if ((*cmp_eq)(key, KEY(next)) == 0) {
                    /* keys match */
                    search_result result = {.status = SEARCH_SUCCESS,
                                            .anchor = copy,
                                            .value = next,
                                            .hash = hash};
                    return result;
                }
                /* not found: same hash but different key */
                search_result result = {.status = SEARCH_FAIL_KEYMISMATCH,
                                        .anchor = copy,
                                        .value = next,
                                        .hash = hash};
                return result;
            } else {
                /* For table entries, iterate */
                assert(TABLE(next) != NULL &&
                       "invariant: table ptrs must not be NULL");
                anchor = next;
                hash = hash_next(hash);
                path = path ? next : NULL;
                continue;
            }
        }
        /* expected index is not set, terminate search */
        search_result result = {.status = SEARCH_FAIL_NOTFOUND,
                                .anchor = copy,
                                .value = NULL,
                                .hash = hash};
        return result;
    }
}

const void *hamt_get(const HAMT trie, void *key)
{
    hash_state *hash = &(hash_state){.key = key,
                                     .hash_fn = trie->key_hash,
                                     .hash = trie->key_hash(key, 0),
                                     .depth = 0,
                                     .shift = 0};
    search_result sr = search_iterative(trie->root, hash, trie->key_cmp, key,
                                        NULL, trie->ator);
    if (sr.status == SEARCH_SUCCESS) {
        return untagged(sr.VALUE(value));
    }
    return NULL;
}

static const hamt_node *set(HAMT h, hamt_node *anchor, hamt_key_hash_fn hash_fn,
                            hamt_cmp_fn cmp_fn, void *key, void *value)
{
    hash_state *hash = &(hash_state){.key = key,
                                     .hash_fn = hash_fn,
                                     .hash = hash_fn(key, 0),
                                     .depth = 0,
                                     .shift = 0};
    search_result sr =
        search_iterative(anchor, hash, cmp_fn, key, NULL, h->ator);
    const hamt_node *inserted;
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
    const hamt_node *n =
        set(trie, trie->root, trie->key_hash, trie->key_cmp, key, value);
    return VALUE(n);
}

static path_result search(hamt_node *anchor, hash_state *hash,
                          hamt_cmp_fn cmp_eq, const void *key,
                          struct hamt_allocator *ator)
{
    path_result pr;
    pr.root = mem_alloc(ator, sizeof(hamt_node));
    pr.sr = search_iterative(anchor, hash, cmp_eq, key, pr.root, ator);
    return pr;
}

const HAMT hamt_pset(HAMT h, void *key, void *value)
{
    hash_state *hash = &(hash_state){.key = key,
                                     .hash_fn = h->key_hash,
                                     .hash = h->key_hash(key, 0),
                                     .depth = 0,
                                     .shift = 0};
    HAMT cp = hamt_dup(h);
    path_result pr = search(h->root, hash, h->key_cmp, key, h->ator);
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

static remove_result rem_recursive(hamt_node *root, hamt_node *anchor,
                                   hash_state *hash, hamt_cmp_fn cmp_eq,
                                   const void *key, hamt_node *path,
                                   struct hamt_allocator *ator)
{
    assert(!is_value(VALUE(anchor)) &&
           "Invariant: removal requires an internal node");
    /* copy the table we're pointing to */
    hamt_node *copy = path;
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
        hamt_node *next = &TABLE(copy)[pos];
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
                    hamt_node *other = &TABLE(copy)[!pos];
                    if (is_value(VALUE(other))) {
                        copy = table_gather(ator, copy, !pos);
                        return (remove_result){.status = REMOVE_GATHERED,
                                               .value = value};
                    } else {
                        /* otherwise shrink the node to n_rows == 1 */
                        copy = table_shrink(ator, copy, n_rows, expected_index,
                                            pos);
                    }
                }
                return (remove_result){.status = REMOVE_SUCCESS,
                                       .value = value};
            }
            /* not found: same hash but different key */
            return (remove_result){.status = REMOVE_NOTFOUND, .value = NULL};
        } else {
            /* For table entries, recurse to the next level */
            assert(TABLE(next) != NULL &&
                   "invariant: table ptrs must not be NULL");
            remove_result result =
                rem_recursive(root, next, hash_next(hash), cmp_eq, key,
                              path ? next : NULL, ator);
            if (next != TABLE(root) && result.status == REMOVE_GATHERED) {
                /* remove dangling internal nodes: check if we need to
                 * propagate the gathering of the key-value entry */
                int n_rows = get_popcount(INDEX(copy));
                if (n_rows == 1) {
                    copy = table_gather(ator, copy, 0);
                    return (remove_result){.status = REMOVE_GATHERED,
                                           .value = result.value};
                }
            }
            return (remove_result){.status = REMOVE_SUCCESS,
                                   .value = result.value};
        }
    }
    return (remove_result){.status = REMOVE_NOTFOUND, .value = NULL};
}

static path_result rem(hamt_node *root, hamt_node *anchor, hash_state *hash,
                       hamt_cmp_fn cmp_eq, const void *key,
                       struct hamt_allocator *ator)
{
    path_result pr;
    pr.root = mem_alloc(ator, sizeof(hamt_node));
    pr.rr = rem_recursive(pr.root, anchor, hash, cmp_eq, key, pr.root, ator);
    return pr;
}

void *hamt_remove(HAMT trie, void *key)
{
    hash_state *hash = &(hash_state){.key = key,
                                     .hash_fn = trie->key_hash,
                                     .hash = trie->key_hash(key, 0),
                                     .depth = 0,
                                     .shift = 0};
    remove_result rr = rem_recursive(trie->root, trie->root, hash,
                                     trie->key_cmp, key, NULL, trie->ator);
    if (rr.status == REMOVE_SUCCESS || rr.status == REMOVE_GATHERED) {
        trie->size -= 1;
        return untagged(rr.value);
    }
    return NULL;
}

const HAMT hamt_premove(const HAMT trie, void *key)
{
    hash_state *hash = &(hash_state){.key = key,
                                     .hash_fn = trie->key_hash,
                                     .hash = trie->key_hash(key, 0),
                                     .depth = 0,
                                     .shift = 0};
    HAMT cp = hamt_dup(trie);
    path_result pr =
        rem(trie->root, trie->root, hash, trie->key_cmp, key, trie->ator);
    cp->root = pr.root;
    if (pr.rr.status == REMOVE_SUCCESS || pr.rr.status == REMOVE_GATHERED) {
        cp->size -= 1;
    }
    return cp;
}
void delete (hamt_node *anchor, struct hamt_allocator *ator)
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

struct hamt_iterator_item {
    hamt_node *anchor;
    size_t pos;
    struct hamt_iterator_item *next;
};

struct hamt_iterator_impl {
    HAMT trie;
    hamt_node *cur;
    struct hamt_iterator_item *head, *tail;
};

static struct hamt_iterator_item *
iterator_push_item(hamt_iterator it, hamt_node *anchor, size_t pos)
{
    /* append at the end */
    struct hamt_iterator_item *new_item =
        mem_alloc(it->trie->ator, sizeof(struct hamt_iterator_item));
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

static struct hamt_iterator_item *iterator_peek_item(hamt_iterator it)
{
    return it->head;
}

static struct hamt_iterator_item *iterator_pop_item(hamt_iterator it)
{
    /* pop from front */
    struct hamt_iterator_item *top = it->head;
    it->head = it->head->next;
    return top;
}

hamt_iterator hamt_it_create(const HAMT trie)
{
    struct hamt_iterator_impl *it =
        mem_alloc(trie->ator, sizeof(struct hamt_iterator_impl));
    it->trie = trie;
    it->cur = NULL;
    it->head = it->tail = NULL;
    iterator_push_item(it, trie->root, 0);
    it->head = it->tail;
    hamt_it_next(it);
    return it;
}

void hamt_it_delete(hamt_iterator it)
{
    struct hamt_iterator_item *p = it->head;
    struct hamt_iterator_item *tmp;
    while (p) {
        tmp = p;
        p = p->next;
        mem_free(it->trie->ator, tmp);
    }
    mem_free(it->trie->ator, it);
}

inline bool hamt_it_valid(hamt_iterator it) { return it->cur != NULL; }

hamt_iterator hamt_it_next(hamt_iterator it)
{
    struct hamt_iterator_item *p;
    while (it && (p = iterator_peek_item(it)) != NULL) {
        int n_rows = get_popcount(INDEX(p->anchor));
        for (int i = p->pos; i < n_rows; ++i) {
            hamt_node *cur = &TABLE(p->anchor)[i];
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

const void *hamt_it_get_key(hamt_iterator it)
{
    if (it->cur) {
        return KEY(it->cur);
    }
    return NULL;
}

const void *hamt_it_get_value(hamt_iterator it)
{
    if (it->cur) {
        return untagged(VALUE(it->cur));
    }
    return NULL;
}
