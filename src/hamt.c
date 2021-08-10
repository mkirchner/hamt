#include "hamt.h"

#include <stdint.h>
#include <stdlib.h>

/* Pointer tagging */
#define HAMT_TAG_MASK 0x3 /* last two bits */
#define HAMT_TAG_LEAF 0x1

#define tag(__p) (HamtNode *)((uintptr_t)__p | HAMT_TAG_LEAF)
#define untag(__p) (HamtNode *)((uintptr_t)__p & ~HAMT_TAG_MASK)
#define is_leaf(__p) (((uintptr_t)__p & HAMT_TAG_MASK) == HAMT_TAG_LEAF)

static int get_popcount(uint32_t n) { return __builtin_popcount(n); }

static int get_compact_index(uint32_t sparse_index, uint32_t bitmap)
{
    return get_popcount(bitmap & ((1 << sparse_index) - 1));
}



void* hamt_get(const HAMT* trie, uint32_t key)
{
    return NULL;
}

int hamt_set(HAMT* trie, uint32_t key, void* value)
{
    return 0;
}

void* hamt_delete(HAMT* trie, uint32_t key)
{
    return NULL;
}
