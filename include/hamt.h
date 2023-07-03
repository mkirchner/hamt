#ifndef HAMT_H
#define HAMT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int (*hamt_cmp_fn)(const void *lhs, const void *rhs);
typedef uint32_t (*hamt_key_hash_fn)(const void *key, const size_t gen);

struct hamt;

struct hamt_allocator {
    void *(*malloc)(const size_t size);
    void *(*realloc)(void *chunk, const size_t size);
    void (*free)(void *chunk);
};

extern struct hamt_allocator hamt_allocator_default;

struct hamt *hamt_create(hamt_key_hash_fn key_hash, hamt_cmp_fn key_cmp,
                 struct hamt_allocator *ator);
void hamt_delete(struct hamt *trie);

const void *hamt_get(const struct hamt *trie, void *key);
const void *hamt_set(struct hamt *trie, void *key, void *value);
const struct hamt *hamt_pset(const struct hamt *trie, void *key, void *value);
void *hamt_remove(struct hamt *trie, void *key);
const struct hamt *hamt_premove(const struct hamt *trie, void *key);
size_t hamt_size(const struct hamt *trie);

struct hamt_iterator;

struct hamt_iterator * hamt_it_create(const struct hamt *trie);
void hamt_it_delete(struct hamt_iterator * it);
bool hamt_it_valid(struct hamt_iterator * it);
struct hamt_iterator * hamt_it_next(struct hamt_iterator * it);
const void *hamt_it_get_key(struct hamt_iterator * it);
const void *hamt_it_get_value(struct hamt_iterator * it);

#endif /* HAMT_H */
