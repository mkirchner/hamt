#ifndef HAMT_H
#define HAMT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int (*hamt_cmp_fn)(const void *lhs, const void *rhs);
typedef uint32_t (*hamt_key_hash_fn)(const void *key, const size_t gen);

typedef struct hamt_impl *HAMT;

struct hamt_allocator {
    void *(*malloc)(const size_t size);
    void *(*realloc)(void *chunk, const size_t size);
    void (*free)(void *chunk);
};

extern struct hamt_allocator hamt_allocator_default;

HAMT hamt_create(hamt_key_hash_fn key_hash, hamt_cmp_fn key_cmp,
                 struct hamt_allocator *ator);
void hamt_delete(HAMT trie);

void hamt_cache_init(HAMT trie, size_t cachesize);
void hamt_cache_destroy(HAMT trie);

const void *hamt_get(const HAMT trie, void *key);
const void *hamt_set(HAMT trie, void *key, void *value);
const HAMT hamt_pset(const HAMT trie, void *key, void *value);
void *hamt_remove(HAMT trie, void *key);
const HAMT hamt_premove(const HAMT trie, void *key);
size_t hamt_size(const HAMT trie);

typedef struct hamt_iterator_impl *hamt_iterator;

hamt_iterator hamt_it_create(const HAMT trie);
void hamt_it_delete(hamt_iterator it);
bool hamt_it_valid(hamt_iterator it);
hamt_iterator hamt_it_next(hamt_iterator it);
const void *hamt_it_get_key(hamt_iterator it);
const void *hamt_it_get_value(hamt_iterator it);

#endif /* HAMT_H */
