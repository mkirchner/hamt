#include "minunit.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/cache.c"
#include "hamt.h"

void **shuffle_ptr_array(ptrdiff_t size, void *array[size])
{
    void *tmp;
    for (size_t i = size - 1; i > 0; --i) {
        size_t j = drand48() * (i + 1);
        tmp = array[i];
        array[i] = array[j];
        array[j] = tmp;
    }
    return array;
}

MU_TEST_CASE(test_create_delete)
{
    printf("Testing cache create/delete...\n");
    struct hamt_table_cache_config cfg = {
        .backing_allocator = &hamt_allocator_default,
        .bucket_count = hamt_table_cache_config_default_bucket_count,
        .initial_bucket_sizes = hamt_table_cache_default_bucket_sizes};
    struct hamt_table_cache *cache = hamt_table_cache_create(&cfg);

    MU_ASSERT(cache->backing_allocator == &hamt_allocator_default,
              "backing allocator should point to default allocator");
    for (size_t i = 0; i < 32; ++i) {
        MU_ASSERT(cache->pools[i].size == 0,
                  "initial number of allocations should be zero");
        MU_ASSERT(cache->pools[i].table_size == i + 1, "wrong table size");
        MU_ASSERT(cache->pools[i].buf_ix == 0,
                  "high water mark should start at zero");
        MU_ASSERT(cache->pools[i].chunk != NULL, "chunk should not be NULL");
        MU_ASSERT(cache->pools[i].chunk_count == 1, "expect a single chunk");
        MU_ASSERT(cache->pools[i].chunk->next == NULL,
                  "expect a single chunk at init");
        MU_ASSERT(cache->pools[i].chunk->size ==
                      (i + 1) * hamt_table_cache_default_bucket_sizes[i],
                  "initial chunk size should be table size times default "
                  "bucket size");
    }
    hamt_table_cache_delete(cache);
    return 0;
}

MU_TEST_CASE(test_allocator_stride)
{
    printf("Testing allocator stride...\n");
    struct hamt_table_cache_config cfg = {
        .backing_allocator = &hamt_allocator_default,
        .bucket_count = hamt_table_cache_config_default_bucket_count,
        .initial_bucket_sizes = hamt_table_cache_default_bucket_sizes};
    struct hamt_table_cache *cache = hamt_table_cache_create(&cfg);

    for (size_t i = 0; i < 32; ++i) {
        ptrdiff_t expected_stride = (i + 1) * sizeof(struct hamt_node);
        char *p = (char *)hamt_table_cache_alloc(cache, i + 1);
        ptrdiff_t tables_per_chunk = cache->pools[i].chunk->size / (i + 1);
        /*
        printf("  table size %lu, expected stride %lu, testing %lu tables\n",
                i+1, expected_stride, tables_per_chunk);
        */
        for (size_t j = 0; j < tables_per_chunk - 1; ++j) {
            char *q = (char *)hamt_table_cache_alloc(cache, i + 1);
            MU_ASSERT(q - p == expected_stride, "wrong stride");
            p = q;
        }
    }
    hamt_table_cache_delete(cache);
    return 0;
}

MU_TEST_CASE(test_freelist_addressing)
{
    /* Test addressing, strides, address calculations */
    printf("Testing freelist addressing...\n");

    /* set all cache chunks to the same number of items */
    ptrdiff_t bucket_sizes[32] = {32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32,
                                  32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32,
                                  32, 32, 32, 32, 32, 32, 32, 32, 32, 32};
    struct hamt_table_cache_config cfg = {
        .backing_allocator = &hamt_allocator_default,
        .bucket_count = hamt_table_cache_config_default_bucket_count,
        .initial_bucket_sizes = bucket_sizes};

    /* note: 1-based loops */
    for (size_t n_rows = 1; n_rows < 33; ++n_rows) {
        for (size_t n_chunks = 1; n_chunks < 5; ++n_chunks) {
            /* create a new pool every time for isolated testing */
            struct hamt_table_cache *cache = hamt_table_cache_create(&cfg);

            /* Create an array to hold the pointers to all entries.
             *
             * Chunk sizes double as new chunks get allocated, therefore
             * the number of entries in chunk k is
             *     n_k = 2^(k-1) * chunk_size
             * where k is 1-based. The total number of pointers is the
             * sum of n_k for k in 1..4 */

            ptrdiff_t n_pointers = 0;
            for (ptrdiff_t k = 0; k < n_chunks; ++k) {
                n_pointers += (1 << k) * bucket_sizes[n_rows - 1];
            }
            struct hamt_node **ptrs;
            ptrs = malloc(n_pointers * sizeof(struct hamt_node *));

            /* deplete the cache and trigger resize until hitting the correct
             * capacity */
            for (size_t pi = 0; pi < n_pointers; ++pi) {
                ptrs[pi] = hamt_table_cache_alloc(cache, n_rows);
            }

            /* make sure we created the correct number of chunks */
            struct table_allocator *ator = &cache->pools[n_rows - 1];
            MU_ASSERT(ator->chunk_count == n_chunks,
                      "Invalid number of chunks");

            /* make sure the number of pointers corresponds to the cache size */
            MU_ASSERT(ator->size == n_pointers,
                      "Expeceted and actual # items differ");

            /* and also to the total capacity */
            ptrdiff_t n_slots = 0;
            struct table_allocator_chunk *chunk = ator->chunk;
            for (size_t c = 0; c < ator->chunk_count; ++c) {
                n_slots += chunk->size / n_rows;
                chunk = chunk->next;
            }
            MU_ASSERT(n_slots == n_pointers, "Failed to exhaust cache");

            /* no dangling chunks! */
            MU_ASSERT(chunk == NULL, "More than chunk_count chunks in pool");

            /* shuffle the pointer array */
            shuffle_ptr_array(n_pointers, (void **)ptrs);

            /* return all pointers to the freelist in shuffled order */
            for (size_t pi = 0; pi < n_pointers; ++pi) {
                hamt_table_cache_free(cache, n_rows, ptrs[pi]);
            }

            /* walk the freelist (don't just use the pointer array, test
             * the freelist's ability to properly chain things instead)
             * and set every entry to { 0x42 } */
            struct table_allocator_freelist *fl = ator->fl;
            while (fl) {
                struct table_allocator_freelist *next = fl->next;
                memset(fl, 0x42, n_rows * sizeof(struct hamt_node));
                fl = next;
            }

            /* iterate over all chunks and assert that every byte
             * is set to 0x42 */
            chunk = ator->chunk;
            for (size_t c = 0; c < ator->chunk_count; ++c) {
                for (size_t b = 0; b < chunk->size * sizeof(struct hamt_node);
                     ++b) {
                    /* byte-wise addressing hence char* */
                    MU_ASSERT(((char *)chunk->buf)[b] == 0x42,
                              "Unexpected value in chunk");
                }
            }
            /* destroy the pool */
            hamt_table_cache_delete(cache);
        }
    }
    return 0;
}

int mu_tests_run = 0;

MU_TEST_SUITE(test_suite)
{
    MU_RUN_TEST(test_create_delete);
    MU_RUN_TEST(test_allocator_stride);
    MU_RUN_TEST(test_freelist_addressing);
    return 0;
}

int main(void)
{
    printf("---=[ Table cache tests\n");
    char *result = test_suite();
    if (result != 0) {
        printf("%s\n", result);
    } else {
        printf("All tests passed.\n");
    }
    printf("Tests run: %d\n", mu_tests_run);
    return result != 0;
}
