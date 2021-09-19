#ifndef HAMT_H
#define HAMT_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef int (*HamtCmpFn)(const void *lhs, const void *rhs);
typedef uint32_t (*HamtKeyHashFn)(const void *key, const size_t gen);

typedef struct HamtImpl *HAMT;

HAMT hamt_create(HamtKeyHashFn key_hash, HamtCmpFn key_cmp);
void hamt_delete(HAMT);

const void *hamt_get(const HAMT trie, void *key);
const void *hamt_set(HAMT trie, void *key, void *value);
void *hamt_remove(HAMT trie, void *key);

#endif /* HAMT_H */
