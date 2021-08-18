#include "hamt.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "murmur3.h"

/* Pointer tagging */
#define HAMT_TAG_MASK 0x3 /* last two bits */
#define HAMT_TAG_VALUE 0x1

#define tagged(__p) (HamtNode *)((uintptr_t)__p | HAMT_TAG_VALUE)
#define untagged(__p) (HamtNode *)((uintptr_t)__p & ~HAMT_TAG_MASK)
#define is_value(__p) (((uintptr_t)__p & HAMT_TAG_MASK) == HAMT_TAG_VALUE)

static int get_popcount(uint32_t n) { return __builtin_popcount(n); }

static int get_compact_index(uint32_t sparse_index, uint32_t bitmap)
{
    return get_popcount(bitmap & ((1 << sparse_index) - 1));
}

HAMT *hamt_create(HamtCmpEqFn cmp_eq)
{
    HAMT *trie = calloc(sizeof(HAMT), 1);
    trie->cmp_eq = cmp_eq;
    memset(&trie->root, 0, sizeof(HamtNode));
    return trie;
}

typedef enum {
    SEARCH_SUCCESS,
    SEARCH_FAIL_NOTFOUND,
    SEARCH_FAIL_KEYMISMATCH,
    SEARCH_STATUS_MAX
} SearchStatus;

typedef struct SearchResult {
    SearchStatus status;
    const HamtNode *anchor;
    const HamtNode *value;
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
    size_t shift = depth * 5;
    uint32_t expected_index = (hash >> shift) & 0x1f; /* mask last 5 bits */
    //printf("depth: %d, expected_index: %lu\n", depth, expected_index);
    /* check if the expected index is set */
    if (has_index(anchor, expected_index)) {
        /* if yes, get the compact index to address the array */
        int pos = get_compact_index(expected_index, anchor->as.table.index);

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
                                   .value = NULL};
            return result;
        } else {
            /* For table entries, recurse to the next level */
            assert(next->as.table.ptr != NULL &&
                   "invariant: table ptrs must not be NULL");
            return search(next, hash, key, keylen, depth + 1, cmp_eq);
        }
    }
    /* expected index is not set, terminate search */
    SearchResult result = {
        .status = SEARCH_FAIL_NOTFOUND, .anchor = anchor, .value = NULL};
    return result;
}

const void *hamt_get(const HAMT *trie, void *key, size_t keylen)
{
    uint32_t hash = murmur3_32((uint8_t *)key, keylen, trie->seed);
    SearchResult sr = search(&trie->root, hash, key, keylen, 0, trie->cmp_eq);
    if (sr.status == SEARCH_SUCCESS) {
        return sr.value;
    }
    return NULL;
}

int hamt_set(HAMT *trie, void *key, size_t keylen, void *value, size_t len)
{
    // hash the key
    uint32_t hash = murmur3_32((uint8_t *)key, keylen, trie->seed);

    return 0;
}

void *hamt_remove(HAMT *trie, void *key, size_t keylen) { return NULL; }
