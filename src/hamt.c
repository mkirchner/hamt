#include "hamt.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "mem.h"
#include "murmur3.h"

/* Pointer tagging */
#define HAMT_TAG_MASK 0x3 /* last two bits */
#define HAMT_TAG_VALUE 0x1

#define tagged(__p) (HamtNode *)((uintptr_t)__p | HAMT_TAG_VALUE)
#define untagged(__p) (HamtNode *)((uintptr_t)__p & ~HAMT_TAG_MASK)
#define is_value(__p) (((uintptr_t)__p & HAMT_TAG_MASK) == HAMT_TAG_VALUE)

static inline uint32_t get_index(uint32_t hash, size_t depth)
{
    return (hash >> (depth * 5)) & 0x1f; /* mask last 5 bits */
}

static int get_popcount(uint32_t n) { return __builtin_popcount(n); }

static int get_pos(uint32_t sparse_index, uint32_t bitmap)
{
    return get_popcount(bitmap & ((1 << sparse_index) - 1));
}

HAMT *hamt_create(HamtCmpEqFn cmp_eq, uint32_t seed)
{
    HAMT *trie = mem_alloc(sizeof(HAMT));
    memset(&trie->root, 0, sizeof(HamtNode));
    trie->cmp_eq = cmp_eq;
    trie->seed = seed;
    trie->size = 0;
    return trie;
}

typedef enum {
    SEARCH_SUCCESS,
    SEARCH_FAIL_NOTFOUND,
    SEARCH_FAIL_KEYMISMATCH
} SearchStatus;

typedef struct SearchResult {
    SearchStatus status;
    HamtNode *anchor;
    HamtNode *value;
    size_t depth;
} SearchResult;

static inline bool has_index(const HamtNode *anchor, size_t index)
{
    assert(anchor && "anchor must not be NULL");
    assert(index < 32 && "index must not be larger than 31");
    return anchor->as.table.index & (1 << index);
}

static SearchResult search(const HamtNode *anchor, uint32_t hash,
                           const void *key, size_t keylen, size_t depth,
                           HamtCmpEqFn cmp_eq)
{
    assert(!is_value(anchor->as.kv.value) &&
           "Invariant: search requires an internal node");
    assert(depth <= 6 && "Rehashing not supported yet"); /* FIXME */

    /* determine the expected index in table */
    uint32_t expected_index = get_index(hash, depth);
    // printf("depth: %d, expected_index: %lu\n", depth, expected_index);
    /* check if the expected index is set */
    if (has_index(anchor, expected_index)) {
        /* if yes, get the compact index to address the array */
        int pos = get_pos(expected_index, anchor->as.table.index);

        /* index into the table and check what type of entry we're looking at */
        HamtNode *next = &anchor->as.table.ptr[pos];
        if (is_value(next->as.kv.value)) {
            /* For key/value entries, we have two options:
             *   1. The keys match. Return the anchor pointer and the value
             * pointer.
             *   2. The keys do not match. Return the anchor, set value to NULL.
             */
            if ((*cmp_eq)(key, next->as.kv.key, keylen) == 0) {
                /* keys match */
                SearchResult result = {
                    .status = SEARCH_SUCCESS, .anchor = anchor, .value = next};
                return result;
            }
            /* not found: same hash but different key */
            SearchResult result = {.status = SEARCH_FAIL_KEYMISMATCH,
                                   .anchor = anchor,
                                   .value = next,
                                   .depth = depth};
            return result;
        } else {
            /* For table entries, recurse to the next level */
            assert(next->as.table.ptr != NULL &&
                   "invariant: table ptrs must not be NULL");
            return search(next, hash, key, keylen, depth + 1, cmp_eq);
        }
    }
    /* expected index is not set, terminate search */
    SearchResult result = {.status = SEARCH_FAIL_NOTFOUND,
                           .anchor = anchor,
                           .value = NULL,
                           .depth = depth};
    return result;
}

HamtNode *mem_allocate_table(size_t size)
{
    return (HamtNode *)mem_alloc(size * sizeof(HamtNode));
}

void mem_free_table(HamtNode* ptr, size_t size)
{
    /* this will eventually allow construction of a free list */
    mem_free(ptr);
}

static const HamtNode *insert_kv(HamtNode *anchor, uint32_t hash, size_t depth,
                                 void *key, size_t keylen, void *value)
{
    assert(depth < 7 && "Re-hashing not supported yet");
    /* calculate position in new table */
    uint32_t pos = get_index(hash, depth);
    uint32_t new_index = anchor->as.table.index | (1 << pos);
    int compact_index = get_pos(pos, new_index);
    /* create new table */
    size_t cur_size = get_popcount(anchor->as.table.index);
    size_t new_size = cur_size + 1;
    HamtNode *new_table = mem_allocate_table(new_size);
    if (cur_size > 0) {
        /* copy over table */
        memcpy(&new_table[0], &anchor->as.table.ptr[0],
               compact_index * sizeof(HamtNode));
        /* note: this works since (cur_size - compact_index) == 0 for cases
         * where we're adding the new k/v pair at the end (i.e. memcpy(a, b, 0)
         * is a nop) */
        memcpy(&new_table[compact_index + 1], &anchor->as.table.ptr[compact_index],
               (cur_size - compact_index) * sizeof(HamtNode));
    }
    /* add new k/v pair */
    new_table[compact_index].as.kv.key = key;
    new_table[compact_index].as.kv.value = tagged(value);
    /* update trie */
    anchor->as.table.index = new_index;
    mem_free_table(anchor->as.table.ptr, cur_size);
    anchor->as.table.ptr = new_table;
    /* return a pointer to the inserted k/v pair */
    return &new_table[compact_index];
}

static const HamtNode *insert_table(HamtNode *anchor, uint32_t hash, int seed,
                                    size_t depth, void *key, size_t keylen,
                                    void *value)
{
    /* Collect everything we know about the existing value */ 
    void* x_key = anchor->as.kv.key;
    void* x_value = anchor->as.kv.value; /* tagged (!) value ptr */
    uint32_t x_hash = murmur3_32((uint8_t*)x_key, keylen, seed);

    /* increase depth until the hashes diverge, building a list
     * of tables along the way */
    depth++;
    assert(depth<7 && "Rehashing not supported yet"); /* FIXME */
    uint32_t x_next_index = get_index(x_hash, depth);
    uint32_t next_index = get_index(hash, depth);
    while (x_next_index == next_index) {
        anchor->as.table.ptr = mem_allocate_table(1);
        anchor->as.table.index = (1 << next_index);
        depth++;
        assert(depth<7 && "Rehashing not supported yet");
        x_next_index = get_index(x_hash, depth);
        next_index = get_index(hash, depth);
        anchor = anchor->as.table.ptr;
    }
    /* the hashes are different, let's allocate a table with two
     * entries to store the existing and new values */
    anchor->as.table.ptr = mem_allocate_table(2);
    anchor->as.table.index = (1 << next_index) | (1 << x_next_index);
    printf("next_index=%d <-> x_next_index=%d\n", next_index, x_next_index);
    /* determine the proper position in the allocated table */ 
    int x_pos = get_pos(x_next_index, anchor->as.table.index);
    int pos = get_pos(next_index, anchor->as.table.index);
    printf("pos=%d <-> x_pos=%d\n", pos, x_pos);
    /* fill in the existing value; no need to tag the value pointer
     * since it is already tagged. */
    anchor->as.table.ptr[x_pos].as.kv.key = x_key;
    anchor->as.table.ptr[x_pos].as.kv.value = x_value;
    /* fill in the new key/value pair, tagging the pointer to the
     * new value to mark it as a value ptr */
    anchor->as.table.ptr[pos].as.kv.key = key;
    anchor->as.table.ptr[pos].as.kv.value = tagged(value);

    return &anchor->as.table.ptr[pos];
}

static const HamtNode *set(const HAMT *trie, void *key, size_t keylen,
                           void *value)
{
    uint32_t hash = murmur3_32((uint8_t *)key, keylen, trie->seed);
    SearchResult sr = search(&trie->root, hash, key, keylen, 0, trie->cmp_eq);
    switch (sr.status) {
    case SEARCH_SUCCESS:
        /* FIXME: reconsider this policy */
        sr.value->as.kv.value = tagged(value);
        return sr.value;
    case SEARCH_FAIL_NOTFOUND:
        return insert_kv(sr.anchor, hash, sr.depth, key, keylen, value);
    case SEARCH_FAIL_KEYMISMATCH:
        return insert_table(sr.value, hash, trie->seed, sr.depth, key, keylen, value);
    }
}

const void *hamt_get(const HAMT *trie, void *key, size_t keylen)
{
    uint32_t hash = murmur3_32((uint8_t *)key, keylen, trie->seed);
    SearchResult sr = search(&trie->root, hash, key, keylen, 0, trie->cmp_eq);
    if (sr.status == SEARCH_SUCCESS) {
        return untagged(sr.value->as.kv.value);
    }
    return NULL;
}

int hamt_set(HAMT *trie, void *key, size_t keylen, void *value)
{
    // hash the key
    uint32_t hash = murmur3_32((uint8_t *)key, keylen, trie->seed);
    return 0;
}

void *hamt_remove(HAMT *trie, void *key, size_t keylen) { return NULL; }

static void debug_print(const HamtNode *node, size_t depth)
{
    /* print node*/
    if (!is_value(node->as.kv.value)) {
        printf("%*s%s", (int)depth*2, "", "[ ");
        for (size_t i = 0; i<32; ++i) {
            if (node->as.table.index & (1 << i)) {
                printf("%2lu ", i);
            }
        }
        printf("]\n");
        /* print table */
        int n = get_popcount(node->as.table.index);
        for (int i = 0; i < n; ++i) {
            debug_print(&node->as.table.ptr[i], depth+1);
        }
    } else {
        /* print value */
        printf("%*s(%c, %d)\n", (int)depth*2, "",
                *(char*) node->as.kv.key,
                *(int*) untagged(node->as.kv.value));
    }
}

