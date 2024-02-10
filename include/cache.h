#ifndef HAMT_CACHE_H
#define HAMT_CACHE_H

#include <stddef.h>
#include "hamt.h"

/* Table cache for the Hash Array-Mapped Trie. Opaque. */
struct hamt_table_cache;
/* User-facing configuration options for the table cache. */
struct hamt_table_cache_config {
    ptrdiff_t bucket_count;
    ptrdiff_t *initial_bucket_sizes;  /* in # of tables */
    struct hamt_allocator *backing_allocator;
};
/* Default cache user parameter config */
extern struct hamt_table_cache_config hamt_table_cache_config_default;

struct hamt_table_cache *hamt_table_cache_create(struct hamt_table_cache_config *cfg);
void hamt_table_cache_delete(struct hamt_table_cache *cache);
struct hamt_node *hamt_table_cache_alloc(struct hamt_table_cache *cache, size_t n);
void hamt_table_cache_free(struct hamt_table_cache *cache, size_t n, void *p);
#endif
