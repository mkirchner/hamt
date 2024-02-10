#ifndef HAMT_H
#define HAMT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int (*hamt_key_cmp_fn)(const void *lhs, const void *rhs);
typedef uint32_t (*hamt_key_hash_fn)(const void *key, const size_t gen);

struct hamt;

/*
 * A custom allocator interface. This is similar to the function
 * pointer callback allocator interface exposed in e.g. libavl but
 * additionally enables (1) a user-defined context pointer to allow
 * for user state management; and (2) passing current and/or new size
 * information to free() and realloc() functions for (optional) sized
 * deallocation.
 */
struct hamt_allocator {
    void *(*malloc)(const ptrdiff_t size, void *ctx);
    void *(*realloc)(void *ptr, const ptrdiff_t old_size,
                     const ptrdiff_t new_size, void *ctx);
    void (*free)(void *ptr, const ptrdiff_t size, void *ctx);
    void *ctx;
};

extern struct hamt_allocator hamt_allocator_default;

#if defined(WITH_TABLE_CACHE)
/* Table cache */
struct hamt_table_cache;
#endif

struct hamt_config {
    struct hamt_allocator *ator;
    hamt_key_cmp_fn key_cmp_fn;
    hamt_key_hash_fn key_hash_fn;
#if defined(WITH_TABLE_CACHE)
    struct hamt_table_cache *cache;
#endif /* WITH_TABLE_CACHE */
};

struct hamt *hamt_create(const struct hamt_config *cfg);
void hamt_delete(struct hamt *trie);
const void *hamt_get(const struct hamt *trie, void *key);
const void *hamt_set(struct hamt *trie, void *key, void *value);
const struct hamt *hamt_pset(const struct hamt *trie, void *key, void *value);
void *hamt_remove(struct hamt *trie, void *key);
const struct hamt *hamt_premove(const struct hamt *trie, void *key);
size_t hamt_size(const struct hamt *trie);

struct hamt_iterator;

struct hamt_iterator *hamt_it_create(struct hamt *trie);
void hamt_it_delete(struct hamt_iterator *it);
bool hamt_it_valid(struct hamt_iterator *it);
struct hamt_iterator *hamt_it_next(struct hamt_iterator *it);
const void *hamt_it_get_key(struct hamt_iterator *it);
const void *hamt_it_get_value(struct hamt_iterator *it);

#endif /* HAMT_H */
