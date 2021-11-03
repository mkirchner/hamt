#ifndef HAMT_H
#define HAMT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int (*HamtCmpFn)(const void *lhs, const void *rhs);
typedef uint32_t (*HamtKeyHashFn)(const void *key, const size_t gen);

typedef struct HamtImpl *HAMT;

HAMT hamt_create(HamtKeyHashFn key_hash, HamtCmpFn key_cmp);
void hamt_delete(HAMT);

const void *hamt_get(const HAMT trie, void *key);
const void *hamt_set(HAMT trie, void *key, void *value);
const HAMT hamt_pset(const HAMT trie, void *key, void *value);
void *hamt_remove(HAMT trie, void *key);
const HAMT hamt_premove(const HAMT trie, void *key);
size_t hamt_size(const HAMT trie);

typedef struct HamtIteratorImpl *HamtIterator;

HamtIterator hamt_it_create(const HAMT trie);
void hamt_it_delete(HamtIterator it);
bool hamt_it_valid(HamtIterator it);
HamtIterator hamt_it_next(HamtIterator it);
const void *hamt_it_get_key(HamtIterator it);
const void *hamt_it_get_value(HamtIterator it);

#endif /* HAMT_H */
