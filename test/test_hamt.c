#include "hamt.h"
#include "minunit.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uh.h"
#include "utils.h"
#include "words.h"

#include "../src/cache.c"
#include "../src/hamt.c"
#include "../src/murmur3.c"

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

/*
 * Prints `node` and all its descendants in the HAMT.
 *
 * @param ix Index of `node` in its table (for illustratrion only)
 * @param node Pointer to the anchor node
 * @param depth Tree depth as a parameter, 0-based
 */
static void debug_print_string(size_t ix, const struct hamt_node *node,
                               size_t depth)
{
    if (!node) {
        printf("debug_print_string called on a NULL node\n");
        return;
    }
    /* node can either be a internal (table) node or a leaf (value) node */
    if (!is_value(node->as.kv.value)) {
        /* this is an internal/table node */
        int n = get_popcount(node->as.table.index);
        printf("%*s \\- [ ix=%2lu sz=%2d p=%p: ", (int)depth * 2, "", ix, n,
               (void *)node);
        for (size_t i = 0; i < 32; ++i) {
            if (node->as.table.index & (1 << i)) {
                printf("%2lu(%2i) ", i, get_pos(i, node->as.table.index));
            }
        }
        printf("\n");
        /* recursively descent into subtables */
        for (int i = 0; i < n; ++i) {
            debug_print_string(i, &node->as.table.ptr[i], depth + 1);
        }
    } else {
        /* this is a leaf/value node */
        printf("%*s \\_ (%2lu) @%p: (%s -> %d)\n", (int)depth * 2, "", ix,
               (void *)node, (char *)node->as.kv.key,
               *(int *)untagged(node->as.kv.value));
    }
}

/* helper function to create a HAMT config; only takes subset of key
 * parameters, uses defaults for the the rest */
struct hamt_config *create_config(struct hamt_allocator *allocator,
                                  hamt_key_hash_fn key_hash_fn,
                                  hamt_key_cmp_fn key_cmp_fn)
{
    struct hamt_config *cfg = NULL;
#if defined(WITH_TABLE_CACHE)
    struct hamt_table_cache_config *tc_cfg =
        allocator->malloc(sizeof *tc_cfg, allocator->ctx);
    if (!tc_cfg)
        goto exit;
    *tc_cfg = (struct hamt_table_cache_config){
        .bucket_count = hamt_table_cache_config_default_bucket_count,
        .initial_bucket_sizes = hamt_table_cache_default_bucket_sizes,
        .backing_allocator = allocator};
    struct hamt_table_cache *cache = hamt_table_cache_create(tc_cfg);
    if (!cache)
        goto cleanup_cache_config;
#endif
    cfg = allocator->malloc(sizeof *cfg, allocator->ctx);
    if (cfg) {
        *cfg = (struct hamt_config)
        {
            .ator = allocator,
#if defined(WITH_TABLE_CACHE)
            .cache = cache,
#endif
            .key_cmp_fn = key_cmp_fn, .key_hash_fn = key_hash_fn
        };
    }
#if defined(WITH_TABLE_CACHE)
    else
        goto cleanup_cache;
    goto exit;
cleanup_cache:
    allocator->free(cache, sizeof *cache, allocator->ctx);
cleanup_cache_config:
    allocator->free(tc_cfg, sizeof *tc_cfg, allocator->ctx);
exit:
#endif
    return cfg;
}

void delete_config(struct hamt_config *cfg)
{
    if (cfg) {
#if defined(WITH_TABLE_CACHE)
        if (cfg->cache) {
            free(cfg->cache);
        }
#endif
        free(cfg);
    }
}

static int my_strncmp_1(const void *lhs, const void *rhs)
{
    return strncmp((const char *)lhs, (const char *)rhs, 1);
}

static uint32_t my_hash_1(const void *key, const size_t _)
{
    /* ignore gen here */
    return murmur3_32((uint8_t *)key, 1, 0);
}

static void print_keys(int32_t hash)
{
    for (size_t i = 0; i < 6; ++i) {
        uint32_t key = (hash >> (5 * i)) & 0x1f;
        printf("%2d ", key);
    }
}
static int my_keycmp_string(const void *lhs, const void *rhs)
{
    /* expects lhs and rhs to be pointers to 0-terminated strings */
    size_t nl = strlen((const char *)lhs);
    size_t nr = strlen((const char *)rhs);
    return strncmp((const char *)lhs, (const char *)rhs, nl > nr ? nl : nr);
}

static uint32_t my_keyhash_string(const void *key, const size_t gen)
{
    uint32_t hash = murmur3_32((uint8_t *)key, strlen((const char *)key), gen);
    return hash;
}

static uint32_t my_keyhash_universal(const void *key, const size_t gen)
{
    return sedgewick_universal_hash((const char *)key, 0x8fffffff - (gen << 8));
}

#ifdef WITH_TABLE_CACHE
#ifdef WITH_TABLE_CACHE_STATS
static void print_allocation_stats(struct hamt *t)
{
    ptrdiff_t total_size = 0;
    ptrdiff_t total_allocated_items = 0;
    for (size_t l = 0; l < 32; ++l) {
        total_size += t->cache->pools[l].size;
        total_allocated_items += t->cache->pools[l].size * l;
    }
    printf("    Alloc overhead ratio: %f\n",
           total_allocated_items / (float)t->size);
    printf("    Pool allocator statistics:\n");
    printf("       tsize    psize    psize%%   allocs    frees    fill%%  \n");
    printf("      ------- --------- -------- -------- --------- -------\n");
    for (size_t l = 0; l < 32; ++l) {
        printf("      %6lu  %8lu  %5.2f%%  %7lu  %9lu  %4.2f%% \n", l + 1,
               t->cache->pools[l].size,
               100 * t->cache->pools[l].size / (float)total_size,
               t->cache->pools[l].stats.alloc_count,
               t->cache->pools[l].stats.free_count,
               100 * (1.0 - (t->cache->pools[l].stats.free_count /
                             (float)t->cache->pools[l].stats.alloc_count)));
    }
}
#endif
#endif
MU_TEST_CASE(test_murmur3_x86_32)
{
    printf(". testing Murmur3 (x86, 32bit)\n");
    /* test vectors from
     * https://stackoverflow.com/questions/14747343/murmurhash3-test-vectors */
    struct {
        char *key;
        size_t len;
        uint32_t seed;
        uint32_t expected;
    } test_cases[7] = {
        {NULL, 0, 0, 0},
        {NULL, 0, 1, 0x514e28b7},
        {NULL, 0, 0xffffffff, 0x81f16f39},
        {"\x00\x00\x00\x00", 4, 0, 0x2362f9de},
        {"\xff\xff\xff\xff", 4, 0, 0x76293b50},
        {"\x21\x43\x65\x87", 4, 0, 0xf55b516b},
        {"\x21\x43\x65\x87", 4, 0x5082edee, 0x2362f9de},
    };

    for (size_t i = 0; i < 7; ++i) {
        uint32_t hash = murmur3_32((uint8_t *)test_cases[i].key,
                                   test_cases[i].len, test_cases[i].seed);
        MU_ASSERT(hash == test_cases[i].expected, "Wrong hash");
    }
    return 0;
}
MU_TEST_CASE(cache_test_create_delete)
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

MU_TEST_CASE(cache_test_allocator_stride)
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

MU_TEST_CASE(cache_test_freelist_addressing)
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

MU_TEST_CASE(test_popcount)
{
    printf(". testing popcount\n");
    /* we're relying on a built-in, spot-check a few cases */
    struct {
        uint32_t number;
        uint32_t nbits;
    } test_cases[4] = {{0, 0}, {42, 3}, {1337, 6}, {UINT32_MAX, 32}};

    for (size_t i = 0; i < 4; ++i) {
        MU_ASSERT(get_popcount(test_cases[i].number) == test_cases[i].nbits,
                  "Unexpected number of set bits");
    }
    return 0;
}

MU_TEST_CASE(test_compact_index)
{
    printf(". testing compact index calculation\n");
    /* 32 bits, set 7, 15, 19 */
    uint32_t bitmap = (1 << 7) | (1 << 15) | (1 << 19);
    /* test cases */
    struct {
        uint32_t sparse_index;
        int expected_dense_index;
    } test_cases[8] = {{0, 0},  {6, 0},  {7, 0},  {8, 1},
                       {14, 1}, {16, 2}, {18, 2}, {20, 3}};

    for (size_t i = 0; i < 8; ++i) {
        MU_ASSERT(get_pos(test_cases[i].sparse_index, bitmap) ==
                      test_cases[i].expected_dense_index,
                  "Unexpected dense index");
    }
    return 0;
}

MU_TEST_CASE(test_tagging)
{
    printf(". testing pointer tagging\n");
    struct hamt_node n;
    struct hamt_node *p = &n;
    MU_ASSERT(!is_value(p), "Raw pointer must not be tagged");
    p = tagged(p);
    MU_ASSERT(is_value(p),
              "Tagged pointer should be detected as tagged pointer");
    p = untagged(p);
    MU_ASSERT(!is_value(p), "Untagging must return a raw pointer");
    return 0;
}

MU_TEST_CASE(test_search)
{
    printf(". testing search\n");
    /* Data
    "0" -> d271c07f : 11 01001 00111 00011 10000 00011 11111  [ 31  3 16  3  7
    9 ] "2" -> 0129e217 : 00 00000 10010 10011 11000 10000 10111  [ 23 16 24 19
    18  0 ] "4" -> e131cc88 : 11 10000 10011 00011 10011 00100 01000  [  8  4 19
    3 19 16 ] "7" -> 23ea8628 : 00 10001 11110 10101 00001 10001 01000  [  8 17
    1 21 30 17 ] "8" -> bd920017 : 10 11110 11001 00100 00000 00000 10111  [ 23
    0  0  4 25 30 ]
    */

    char keys[] = "02478c";
    /* uncomment to get the hashes
    char buf[38];
    for (size_t i = 0; i < 6; ++i) {
        uint32_t hash = my_hash_1(&keys[i], 0);
        printf("    %c -> %08x : %s [ ", keys[i], hash, i2b(hash, buf));
        print_keys(hash);
        printf("]\n");
    }
    */

    /*
     * We're now manually building the trie corresponding to the data above:
     *
     * +---+---+   8+---+---+      4+---+---+
     * |   | --+--->|   | --+------>|"4"| 4 |
     * +---+---+  23+---+---+     17+---+---+
     *              |   | --+--+    |"7"| 7 |
     *            31+---+---+  |    +---+---+
     *              |"0"| 0 |  |
     *              +---+---+  |   0+---+---+
     *                         +--->|"8"| 8 |
     *                            16+---+---+
     *                              |"2"| 2 |
     *                              +---+---+
     *
     * Note that the keys and values are actually pointers to the keys and
     * values shown here for brevity.
     * We're also not adding "c" as a key in order to be able to test
     * for a SEARCH_FAIL_KEYMISMATCH case.
     */

    int values[] = {0, 2, 4, 7, 8};

    struct hamt_node *t_8 =
        (struct hamt_node *)calloc(sizeof(struct hamt_node), 2);
    t_8[0].as.kv.key = &keys[2];
    t_8[0].as.kv.value = tagged(&values[2]);
    t_8[1].as.kv.key = &keys[3];
    t_8[1].as.kv.value = tagged(&values[3]);

    struct hamt_node *t_23 =
        (struct hamt_node *)calloc(sizeof(struct hamt_node), 2);
    t_23[0].as.kv.key = &keys[4];
    t_23[0].as.kv.value = tagged(&values[4]);
    t_23[1].as.kv.key = &keys[1];
    t_23[1].as.kv.value = tagged(&values[1]);

    struct hamt_node *t_root =
        (struct hamt_node *)calloc(sizeof(struct hamt_node), 3);
    t_root[0].as.table.index = (1 << 4) | (1 << 17);
    t_root[0].as.table.ptr = t_8;
    t_root[1].as.table.index = (1 << 0) | (1 << 16);
    t_root[1].as.table.ptr = t_23;
    t_root[2].as.kv.key = &keys[0];
    t_root[2].as.kv.value = tagged(&values[0]);

    struct hamt t;
    t.key_cmp = my_strncmp_1;
    t.ator = &hamt_allocator_default;
    t.root = ALLOC(t.ator, sizeof(struct hamt_node));
    t.root->as.table.index = (1 << 8) | (1 << 23) | (1 << 31);
    t.root->as.table.ptr = t_root;

    struct {
        char *key;
        search_status expected_status;
        int expected_value;
    } test_cases[10] = {
        {"0", SEARCH_SUCCESS, 0},       {"1", SEARCH_FAIL_NOTFOUND, 0},
        {"2", SEARCH_SUCCESS, 2},       {"3", SEARCH_FAIL_NOTFOUND, 0},
        {"4", SEARCH_SUCCESS, 4},       {"5", SEARCH_FAIL_NOTFOUND, 0},
        {"6", SEARCH_FAIL_NOTFOUND, 0}, {"7", SEARCH_SUCCESS, 7},
        {"8", SEARCH_SUCCESS, 8},       {"c", SEARCH_FAIL_KEYMISMATCH, 0}};

    for (size_t i = 0; i < 10; ++i) {
        struct hash_state *hash =
            &(struct hash_state){.key = test_cases[i].key,
                                 .hash_fn = my_hash_1,
                                 .hash = my_hash_1(test_cases[i].key, 0),
                                 .depth = 0,
                                 .shift = 0};
        struct search_result sr = search_recursive(
            &t, t.root, hash, my_strncmp_1, test_cases[i].key, NULL);
        MU_ASSERT(sr.status == test_cases[i].expected_status,
                  "Unexpected search result status");
        if (test_cases[i].expected_status == SEARCH_SUCCESS) {
            /* test key */
            MU_ASSERT(0 == my_strncmp_1(test_cases[i].key,
                                        (char *)sr.value->as.kv.key),
                      "Successful search returns non-matching key");
            /* test value */
            MU_ASSERT(test_cases[i].expected_value ==
                          *(int *)untagged(sr.value->as.kv.value),
                      "Successful search returns wrong value");
        }
    }

    free(t_root);
    free(t_23);
    free(t_8);
    free(t.root);
    return 0;
}

MU_TEST_CASE(test_set_with_collisions)
{
    printf(". testing set/insert w/ forced key collision\n");
    struct hamt_config *cfg =
        create_config(&hamt_allocator_default, my_hash_1, my_strncmp_1);
    struct hamt *t = hamt_create(cfg);

    /* example 1: no hash collisions */
    char keys[] = "028";
    int values[] = {0, 2, 8};

    struct hamt_node *t_root = table_allocate(t, 2);
    t_root[0].as.kv.key = &keys[0];
    t_root[0].as.kv.value = tagged(&values[0]);
    t_root[1].as.kv.key = &keys[1];
    t_root[1].as.kv.value = tagged(&values[1]);

    t->root->as.table.ptr = t_root;
    t->root->as.table.index = (1 << 23) | (1 << 31);

    /* insert value and find it again */
    const struct hamt_node *new_node =
        set(t, t->root, t->key_hash, t->key_cmp, &keys[2], &values[2]);
    struct hash_state *hash =
        &(struct hash_state){.key = &keys[2],
                             .hash_fn = t->key_hash,
                             .hash = t->key_hash(&keys[2], 0),
                             .depth = 0,
                             .shift = 0};
    struct search_result sr =
        search_recursive(t, t->root, hash, t->key_cmp, &keys[2], NULL);
    MU_ASSERT(sr.status == SEARCH_SUCCESS, "failed to find inserted value");
    MU_ASSERT(new_node == sr.value, "Query result points to the wrong node");
    hamt_delete(t);
    delete_config(cfg);
    return 0;
}

MU_TEST_CASE(test_set_whole_enchilada_00)
{
    printf(". testing set/insert w/ key collision\n");

    /* test data, see above */
    struct {
        char key;
        int value;
    } data[5] = {{'0', 0}, {'2', 2}, {'4', 4}, {'7', 7}, {'8', 8}};

    struct hamt_config *cfg =
        create_config(&hamt_allocator_default, my_hash_1, my_strncmp_1);
    struct hamt *t = hamt_create(cfg);
    for (size_t i = 0; i < 5; ++i) {
        set(t, t->root, t->key_hash, t->key_cmp, &data[i].key, &data[i].value);
    }

    for (size_t i = 0; i < 5; ++i) {
        struct hash_state *hash =
            &(struct hash_state){.key = &data[i].key,
                                 .hash_fn = t->key_hash,
                                 .hash = t->key_hash(&data[i].key, 0),
                                 .depth = 0,
                                 .shift = 0};
        struct search_result sr =
            search_recursive(t, t->root, hash, t->key_cmp, &data[i].key, NULL);
        MU_ASSERT(sr.status == SEARCH_SUCCESS, "failed to find inserted value");
        int *value = (int *)untagged(sr.value->as.kv.value);
        MU_ASSERT(value, "found value is NULL");
        MU_ASSERT(*value == data[i].value, "value mismatch");
        MU_ASSERT(value == &data[i].value, "value pointer mismatch");
    }
    hamt_delete(t);
    delete_config(cfg);
    return 0;
}

MU_TEST_CASE(test_set_stringkeys)
{
    printf(". testing set/insert w/ string keys\n");

    /* test data, see above */
    struct {
        char *key;
        int value;
    } data[6] = {{"humpty", 1}, {"dumpty", 2}, {"sat", 3},
                 {"on", 4},     {"the", 5},    {"wall", 6}};

    struct hamt_config *cfg = create_config(
        &hamt_allocator_default, my_keyhash_string, my_keycmp_string);
    struct hamt *t = hamt_create(cfg);
    for (size_t i = 0; i < 6; ++i) {
        // printf("setting (%s, %d)\n", data[i].key, data[i].value);
        set(t, t->root, t->key_hash, t->key_cmp, data[i].key, &data[i].value);
        // debug_print_string(t->root, 4);
    }

    for (size_t i = 0; i < 6; ++i) {
        // printf("querying (%s, %d)\n", data[i].key, data[i].value);
        struct hash_state *hash =
            &(struct hash_state){.key = data[i].key,
                                 .hash_fn = t->key_hash,
                                 .hash = t->key_hash(data[i].key, 0),
                                 .depth = 0,
                                 .shift = 0};
        struct search_result sr =
            search_recursive(t, t->root, hash, t->key_cmp, data[i].key, NULL);
        MU_ASSERT(sr.status == SEARCH_SUCCESS, "failed to find inserted value");
        int *value = (int *)untagged(sr.value->as.kv.value);
        MU_ASSERT(value, "found value is NULL");
        // printf("    %s: %d == %d\n", sr.value->as.kv.key, *value,
        // data[i].value);
        MU_ASSERT(*value == data[i].value, "value mismatch");
        MU_ASSERT(value == &data[i].value, "value pointer mismatch");
    }
    hamt_delete(t);
    delete_config(cfg);
    return 0;
}

MU_TEST_CASE(test_aspell_dict_en)
{
    printf(". testing large-scale set/insert w/ string keys\n");

    char **words = NULL;
    struct hamt *t;

    words_load(&words, WORDS_MAX);
    struct hamt_config *cfg = create_config(
        &hamt_allocator_default, my_keyhash_string, my_keycmp_string);
    t = hamt_create(cfg);
    for (size_t i = 0; i < WORDS_MAX; i++) {
        hamt_set(t, words[i], words[i]);
    }

    /* Check if we can retrieve the entire dictionary */
    for (size_t i = 0; i < WORDS_MAX; i++) {
        MU_ASSERT(hamt_get(t, words[i]) != NULL, "could not find expected key");
    }

    /* Check if "bluism" has search depth 6 */
    char target[] = "bluism";
    struct hash_state *hash =
        &(struct hash_state){.key = target,
                             .hash_fn = my_keyhash_string,
                             .hash = my_keyhash_string(target, 0),
                             .depth = 0,
                             .shift = 0};
    struct search_result sr =
        search_recursive(t, t->root, hash, t->key_cmp, target, NULL);
    MU_ASSERT(sr.status == SEARCH_SUCCESS, "fail");
    char *value = (char *)untagged(sr.value->as.kv.value);

    MU_ASSERT(value, "failed to retrieve existing value");
    MU_ASSERT(strcmp(value, target) == 0, "invalid value");
    MU_ASSERT(sr.hash->depth == 6, "invalid depth");

    /*
    printf("  Table size distribution for n=%lu:\n", t->size);
    for (size_t i = 0; i < 32; ++i) {
        printf("    %2lu: %lu, %.4f\n", i+1, t->stats.table_sizes[i],
    t->stats.table_sizes[i]/(double)t->size);
    }
    */
    /*
    printf("{");
    for (size_t i = 0; i < 32; ++i) {
        printf("%.4f, ", t->stats.table_sizes[i]/(double)t->size);
    }
    printf("}\n");
    */

    hamt_delete(t);
    words_free(words, WORDS_MAX);
    return 0;
}
MU_TEST_SUITE(test_setget_large_scale)
{
    printf(". testing set/get/get for 1M items\n");

    size_t n_items = 1e6;
    char **words = NULL;
    words_load_numbers(&words, 0, n_items);

    struct hamt *t;
    struct hamt_config *cfg = create_config(
        &hamt_allocator_default, my_keyhash_string, my_keycmp_string);
    t = hamt_create(cfg);
    for (size_t i = 0; i < n_items; i++) {
        hamt_set(t, words[i], words[i]);
        MU_ASSERT(hamt_get(t, words[i]) == words[i],
                  "Failed to get key we just pushed");
    }
    for (size_t i = 0; i < n_items; i++) {
        MU_ASSERT(hamt_get(t, words[i]), "Failed to get key we pushed earlier");
    }
    hamt_delete(t);
    words_free(words, n_items);
    return 0;
}

MU_TEST_CASE(test_shrink_table)
{
    printf(". testing table operations: shrink\n");
    enum { N = 5 };
    struct {
        char *key;
        int value;
        int index;
    } data[N] = {
        {"0", 0, 1}, {"2", 2, 3}, {"4", 4, 4}, {"7", 7, 12}, {"8", 8, 22}};

    /* dummy struct hamt *so we can pass the allocator info */
    struct hamt_config *cfg = create_config(
        &hamt_allocator_default, my_keyhash_string, my_keycmp_string);
    struct hamt *t = hamt_create(cfg);

    /* create table w/ 5 entries and delete each position */
    struct hamt_node *a0;
    for (size_t delete_pos = 0; delete_pos < N; delete_pos++) {
        a0 = ALLOC(t->ator, sizeof(struct hamt_node));
        memset(a0, 0, sizeof(struct hamt_node));
        TABLE(a0) = table_allocate(t, N);
        for (size_t i = 0; i < N; ++i) {
            INDEX(a0) |= (1 << data[i].index);
            TABLE(a0)[i].as.kv.key = (void *)data[i].key;
            TABLE(a0)[i].as.kv.value = tagged(&data[i].value);
        }
        uint32_t delete_index = data[delete_pos].index;
        a0 = table_shrink(t, a0, N, delete_index, delete_pos);
        MU_ASSERT(get_popcount(INDEX(a0)) == N - 1, "wrong number of rows");
        size_t diff = 0;
        for (size_t i = 0; i < N; ++i) {
            if (i == delete_pos) {
                diff = 1;
                continue;
            }
            MU_ASSERT(data[i].key == TABLE(a0)[i - diff].as.kv.key,
                      "unexpected key in shrunk table");
            MU_ASSERT((void *)&data[i].value ==
                          untagged(TABLE(a0)[i - diff].as.kv.value),
                      "unexpected value in shrunk table");
        }
        table_free(t, TABLE(a0), 4);
        FREE(t->ator, a0, sizeof(struct hamt_node));
    }
    hamt_delete(t);
    delete_config(cfg);
    return 0;
}

MU_TEST_CASE(test_gather_table)
{

    printf(". testing table operations: gather\n");
    enum { N = 2 };
    struct {
        char *key;
        int value;
        int index;
    } data[N] = {{"0", 0, 1}, {"2", 2, 3}};

    /* dummy struct hamt *so we can pass the allocator info */
    struct hamt_config *cfg = create_config(
        &hamt_allocator_default, my_keyhash_string, my_keycmp_string);
    struct hamt *t = hamt_create(cfg);

    struct hamt_node *a0 = ALLOC(t->ator, sizeof(struct hamt_node));
    a0->as.table.index = 0;
    a0->as.table.ptr = table_allocate(t, N);
    for (size_t i = 0; i < N; ++i) {
        a0->as.table.index |= (1 << data[i].index);
        a0->as.table.ptr[i].as.kv.key = (void *)data[i].key;
        a0->as.table.ptr[i].as.kv.value = tagged(&data[i].value);
    }

    struct hamt_node *a1 = table_gather(t, a0, 0);

    MU_ASSERT(a1->as.kv.key == data[0].key, "wrong key in gather");
    MU_ASSERT(untagged(a1->as.kv.value) == (void *)&data[0].value,
              "wrong value in gather");
    delete_config(cfg);
    return 0;
}

MU_TEST_CASE(test_remove)
{
    printf(". testing remove w/ string keys\n");

    enum { N = 6 };
    struct {
        char *key;
        int value;
    } data[N] = {{"humpty", 1}, {"dumpty", 2}, {"sat", 3},
                 {"on", 4},     {"the", 5},    {"wall", 6}};

    struct hamt_config *cfg = create_config(
        &hamt_allocator_default, my_keyhash_string, my_keycmp_string);
    struct hamt *t = hamt_create(cfg);

    for (size_t k = 0; k < 3; ++k) {
        for (size_t i = 0; i < N; ++i) {
            set(t, t->root, t->key_hash, t->key_cmp, data[i].key,
                &data[i].value);
        }
        for (size_t i = 0; i < N; ++i) {
            struct hash_state *hash =
                &(struct hash_state){.key = data[i].key,
                                     .hash_fn = t->key_hash,
                                     .hash = t->key_hash(data[i].key, 0),
                                     .depth = 0,
                                     .shift = 0};
            struct path_result pr =
                rem(t, t->root, t->root, hash, t->key_cmp, data[i].key);
            MU_ASSERT(pr.rr.status == REMOVE_SUCCESS ||
                          pr.rr.status == REMOVE_GATHERED,
                      "failed to find inserted value");
            MU_ASSERT(*(int *)untagged(pr.rr.value) == data[i].value,
                      "wrong value in remove");
        }
    }
    hamt_delete(t);
    delete_config(cfg);
    return 0;
}

MU_TEST_CASE(test_create_delete)
{
    printf(". testing create/delete cycle\n");
    struct hamt *t;
    struct hamt_config *cfg = create_config(
        &hamt_allocator_default, my_keyhash_string, my_keycmp_string);
    t = hamt_create(cfg);
    hamt_delete(t);

    // FIXME: what should we be asserting here?

    t = hamt_create(cfg);
    struct {
        char *key;
        int value;
    } data[6] = {{"humpty", 1}, {"dumpty", 2}, {"sat", 3},
                 {"on", 4},     {"the", 5},    {"wall", 6}};
    for (size_t i = 0; i < 6; ++i) {
        set(t, t->root, t->key_hash, t->key_cmp, data[i].key, &data[i].value);
    }
    hamt_delete(t);
    delete_config(cfg);
    return 0;
}

MU_TEST_CASE(test_size)
{
    printf(". testing tree size tracking\n");
    struct hamt *t;
    struct hamt_config *cfg = create_config(
        &hamt_allocator_default, my_keyhash_string, my_keycmp_string);
    t = hamt_create(cfg);
    enum { N = 6 };
    struct {
        char *key;
        int value;
    } data[N] = {{"humpty", 1}, {"dumpty", 2}, {"sat", 3},
                 {"on", 4},     {"the", 5},    {"wall", 6}};
    for (size_t i = 0; i < N; ++i) {
        hamt_set(t, data[i].key, &data[i].value);
        MU_ASSERT(hamt_size(t) == (i + 1), "Wrong tree size during set");
    }
    for (size_t i = 0; i < N; ++i) {
        hamt_remove(t, data[i].key);
        MU_ASSERT(hamt_size(t) == (N - 1 - i), "Wrong tree size during remove");
    }
    hamt_delete(t);
    delete_config(cfg);
    return 0;
}

MU_TEST_CASE(test_iterators)
{
    printf(". testing iterators\n");
    struct hamt *t;

    struct {
        char *key;
        int value;
    } data[6] = {{"humpty", 1}, {"dumpty", 2}, {"sat", 3},
                 {"on", 4},     {"the", 5},    {"wall", 6}};

    struct {
        char *key;
        int value;
    } expected[6] = {{"the", 5}, {"on", 4},     {"wall", 6},
                     {"sat", 3}, {"humpty", 1}, {"dumpty", 2}};

    struct hamt_config *cfg = create_config(
        &hamt_allocator_default, my_keyhash_string, my_keycmp_string);
    t = hamt_create(cfg);

    /* test create/delete */

    struct hamt_iterator *it = hamt_it_create(t);
    hamt_it_next(it);
    MU_ASSERT(it->cur == NULL, "iteration fail for empty trie");
    hamt_it_delete(it);

    for (size_t i = 0; i < 6; ++i) {
        hamt_set(t, data[i].key, &data[i].value);
    }
    it = hamt_it_create(t);
    size_t count = 0;
    while (hamt_it_valid(it)) {
        MU_ASSERT(strcmp((char *)hamt_it_get_key(it), expected[count].key) == 0,
                  "Unexpected key in iteration");
        MU_ASSERT(*(int *)hamt_it_get_value(it) == expected[count].value,
                  "Unexpected value in iteration");
        count += 1;
        hamt_it_next(it);
    }
    MU_ASSERT(count == 6, "Wrong number of items in iteration");
    hamt_it_delete(it);

    hamt_delete(t);
    delete_config(cfg);
    return 0;
}

MU_TEST_CASE(test_iterators_1m)
{
    /* Creates a HAMT with 1M items; walks the HAMT
     * using an iterator and compares the iterator
     * count with the HAMT tree size. */
    printf(". testing iterators with 1M items\n");
    size_t n_items = 1e6;
    char **words = NULL;
    /* get the data */
    words_load_numbers(&words, 0, n_items);
    /* create and load the struct hamt **/
    struct hamt_config *cfg = create_config(
        &hamt_allocator_default, my_keyhash_string, my_keycmp_string);
    struct hamt *t = hamt_create(cfg);
    for (size_t i = 0; i < n_items; i++) {
        hamt_set(t, words[i], words[i]);
    }
    /* create an iterator, walk the entire trie and
     * count how often we can increment the iterator before
     * exhausting it */
    struct hamt_iterator *it = hamt_it_create(t);
    size_t count = 0;
    while (hamt_it_valid(it)) {
        count += 1;
        hamt_it_next(it);
    }
    /* make sure the iterator sees as many items as
     * the HAMT is aware of */
    // printf("size: %lu, count: %lu\n", hamt_size(t), count);
    MU_ASSERT(count == hamt_size(t), "Wrong number of items in iteration");
    /* clean up */
    hamt_it_delete(it);
    hamt_delete(t);
    delete_config(cfg);
    return 0;
}
MU_TEST_CASE(test_persistent_set)
{
    printf(". testing set/insert w/ structural sharing\n");

    /* test data, see above */
    struct {
        char *key;
        int value;
    } data[6] = {{"humpty", 1}, {"dumpty", 2}, {"sat", 3},
                 {"on", 4},     {"the", 5},    {"wall", 6}};

    struct hamt_config *cfg = create_config(
        &hamt_allocator_default, my_keyhash_string, my_keycmp_string);
    const struct hamt *t = hamt_create(cfg);
    const struct hamt *tmp;
    for (size_t i = 0; i < 6; ++i) {
        tmp = hamt_pset(t, data[i].key, &data[i].value);
        debug_print_string(0, tmp->root, 0);
        printf("---\n");
        MU_ASSERT(hamt_size(tmp) == hamt_size(t) + 1, "wrong trie size");
        for (size_t k = 0; k <= i; k++) {
            if (k < i) {
                /* test if pre-insert keys are still accessible
                 * in the original trie */
                MU_ASSERT(hamt_get(t, data[k].key) == &data[k].value,
                          "failed to find all expected values in existing");
            }
            /* test is pre-insert keys and the new key are accessible
             * in the new trie */
            MU_ASSERT(hamt_get(tmp, data[k].key) == &data[k].value,
                      "failed to find all expected values in copy");
        }
        /* make sure that the new key is not accessible in the
         * existing trie */
        MU_ASSERT(hamt_get(t, data[i].key) == NULL, "unexpected side effect");
        t = tmp;
    }
    /* There is no way to cleanly free the structurally shared
     * tries without garbage collection. Leak them. */
    return 0;
}

MU_TEST_CASE(test_persistent_aspell_dict_en)
{
    printf(". testing large-scale set/insert w/ structural sharing\n");

    char **words = NULL;
    const struct hamt *t;

    words_load(&words, WORDS_MAX);
    struct hamt_config *cfg = create_config(
        &hamt_allocator_default, my_keyhash_string, my_keycmp_string);
    t = hamt_create(cfg);
    for (size_t i = 0; i < WORDS_MAX; i++) {
        /* structural sharing */
        t = hamt_pset(t, words[i], words[i]);
    }

    /* Check if we can retrieve the entire dictionary */
    for (size_t i = 0; i < WORDS_MAX; i++) {
        MU_ASSERT(hamt_get(t, words[i]) != NULL, "could not find expected key");
    }

    words_free(words, WORDS_MAX);
    /* There is no way to cleanly free the structurally shared
     * tries without garbage collection. Leak them. */
    return 0;
}

MU_TEST_CASE(test_table_extend)
{
    printf(". testing table_extend\n");
    struct hamt_config *cfg = create_config(
        &hamt_allocator_default, my_keyhash_string, my_keycmp_string);
    struct hamt *t = hamt_create(cfg);
    MU_ASSERT(get_popcount(INDEX(t->root)) == 0,
              "root should have zero descendants");
    /*
    printf("-0--\n");
    debug_print_string(0, t->root, 0);
    printf("-0--\n");
    */
    struct hamt_node *n = table_extend(t, t->root, 0, 0, 0);
    MU_ASSERT(get_popcount(INDEX(n)) == 1, "size did not increase by 1");
    MU_ASSERT(n == t->root, "anchor should not change");
    /*
    printf("-1--\n");
    debug_print_string(0, t->root, 0);
    printf("-1--\n");
    */
    n = table_shrink(t, t->root, 1, 0, 0);
    MU_ASSERT(get_popcount(INDEX(n)) == 0, "size did not decrease by 1");
    MU_ASSERT(n == t->root, "anchor should not change");
    /*
    printf("-2--\n");
    debug_print_string(0, t->root, 0);
    printf("-2--\n");
    */
    hamt_delete(t);
    delete_config(cfg);
    return 0;
}

MU_TEST_CASE(test_setget_zero)
{
    printf(". testing setget for a size 0 tree\n");

    /* create a standard HAMT with string keys */
    struct hamt *t;
    struct hamt_config *cfg = create_config(
        &hamt_allocator_default, my_keyhash_string, my_keycmp_string);
    t = hamt_create(cfg);
    /* Add a single key.
     *
     * C does not provide alignment guarantees for static char arrays; advise
     * the compiler to align at a 64 bit boundary to make sure that the value
     * ponter we point to actually supports pointer tagging...
     */
    char key[] __attribute__((aligned(8))) = "the_key";
    char value[] __attribute__((aligned(8))) = "the_value";

    const char *val = hamt_set(t, key, value);
    MU_ASSERT(hamt_size(t) == 1, "wrong size after set");
    MU_ASSERT(strcmp(val, value) == 0, "values are not the same");
    MU_ASSERT(val == value, "value should point to the original value");
    /* make sure we can find it */
    val = hamt_get(t, key);
    MU_ASSERT(val != NULL, "key should be present");
    MU_ASSERT(val == value, "found value should point to the original value");
    /* delete it */
    val = hamt_remove(t, key);
    MU_ASSERT(hamt_size(t) == 0, "wrong size after remove");
    MU_ASSERT(val != NULL, "key should be present");
    MU_ASSERT(val == value,
              "remove return value should point to the original value");
    /* make sure it's gone */
    val = hamt_get(t, key);
    MU_ASSERT(val == NULL, "key should not be present anymore");

    hamt_delete(t);
    delete_config(cfg);
    return 0;
}
MU_TEST_CASE(test_persistent_setget_one)
{
    printf(". testing add/remove of a single element w/ structural sharing\n");

    /* create a standard HAMT with string keys */
    struct hamt_config *cfg = create_config(
        &hamt_allocator_default, my_keyhash_string, my_keycmp_string);
    const struct hamt *t;
    t = hamt_create(cfg);
    /* add a single key */
    char key[] __attribute__((aligned(8))) = "the_key";
    char value[] __attribute__((aligned(8))) = "the_value";
    t = hamt_pset(t, key, value);
    MU_ASSERT(hamt_size(t) == 1, "wrong size after set");
    /* make sure we can find it */
    char *val = (char *)hamt_get(t, key);
    MU_ASSERT(val != NULL, "key should be present");
    MU_ASSERT(val == value, "value should point to the original value");
    /* remove it and make sure (1) it's gone from the new copy and
     * still present in the old */
    const struct hamt *s;
    s = hamt_premove(t, key);
    MU_ASSERT(hamt_get(t, key) != NULL,
              "key should still be present original trie");
    MU_ASSERT(hamt_get(s, key) == NULL,
              "key should have been removed from copy");
    /*
     * There is no way to cleanly free the structurally shared
     * tries without garbage collection. Leak them.
     */
    return 0;
}

MU_TEST_CASE(test_persistent_remove_aspell_dict_en)
{
    printf(". testing large-scale remove w/ structural sharing\n");

    char **words = NULL;
    const struct hamt *t;

    words_load(&words, WORDS_MAX);
    struct hamt_config *cfg = create_config(
        &hamt_allocator_default, my_keyhash_string, my_keycmp_string);
    t = hamt_create(cfg);
    for (size_t i = 0; i < WORDS_MAX; i++) {
        /* structural sharing */
        t = hamt_pset(t, words[i], words[i]);
    }

    /*
     * Delete all entries one by one. After each deletion, check that the
     * deleted value is not present in the new trie and can still be accessed
     * in the previous tree.
     */
    const struct hamt *s;
    for (size_t i = 0; i < WORDS_MAX; i++) {
        /* structural sharing */
        s = hamt_premove(t, words[i]);
        // printf("i=%lu of %lu (pre-size=%lu, post-size=%lu)\n", i, WORDS_MAX,
        // hamt_size(t), hamt_size(s));
        MU_ASSERT(hamt_get(t, words[i]) != NULL,
                  "key should not have been removed from original trie");
        /*
        if (i > (WORDS_MAX - 5)) {
            printf("--- remainder at size %lu:\n", hamt_size(s));
            debug_print_string(0, s->root, 0);
        }
        */

        MU_ASSERT(hamt_get(s, words[i]) == NULL,
                  "key should have been removed from copy");
        /* leak the previous version */
        t = s;
    }

    words_free(words, WORDS_MAX);
    /*
     * There is no way to cleanly free the structurally shared
     * tries without garbage collection. Leak them.
     */
    return 0;
}

MU_TEST_CASE(test_tree_depth)
{
    printf(". creating tree statistics\n");

    size_t n_items = 1e5;
    char **words = NULL;
    struct hamt *t;

    words_load_numbers(&words, 0, n_items);

    hamt_key_hash_fn hash_fns[2] = {my_keyhash_string, my_keyhash_universal};
    char *hash_names[2] = {"murmur3", "sedgewick_universal"};

    for (size_t k = 0; k < 2; ++k) {

        struct hamt_config *cfg = create_config(&hamt_allocator_default,
                                                hash_fns[k], my_keycmp_string);
        t = hamt_create(cfg);
        for (size_t i = 0; i < n_items; i++) {
            hamt_set(t, words[i], words[i]);
        }
        printf("\n  [ %s ]\n", hash_names[k]);
#ifdef WITH_TABLE_CACHE
#ifdef WITH_TABLE_CACHE_STATS
        print_allocation_stats(t);
#endif
#endif

        /* Calculate the avg tree depth across all items */
        double avg_depth = 0.0;
        size_t max_depth = 0;
        for (size_t i = 0; i < n_items; i++) {
            struct hash_state *hash =
                &(struct hash_state){.key = words[i],
                                     .hash_fn = hash_fns[k],
                                     .hash = hash_fns[k](words[i], 0),
                                     .depth = 0,
                                     .shift = 0};
            struct search_result sr =
                search_recursive(t, t->root, hash, t->key_cmp, words[i], NULL);
            if (sr.status != SEARCH_SUCCESS) {
                printf("tree search failed for: %s\n", words[i]);
                continue;
            }
            // in order to calculate depth, item must exist
            MU_ASSERT(sr.status == SEARCH_SUCCESS, "tree depth search failure");
            avg_depth = (avg_depth * i + sr.hash->depth) / (i + 1);
            if (sr.hash->depth > max_depth) {
                max_depth = sr.hash->depth;
                // printf("New max depth %lu for %s\n", max_depth, words[i]);
            }
            /*
            else
            if (sr.hash->depth == max_depth) {
                printf("Equal max depth %lu for %s\n", max_depth, words[i]);
            }
            */
        }
        printf("    Avg depth for %lu items: %0.3f, expected %0.3f, max: %lu\n",
               n_items, avg_depth, log2(n_items) / 5.0,
               max_depth); /* log_32(n_items) */
        hamt_delete(t);
        delete_config(cfg);
    }
    words_free(words, n_items);
    return 0;
}
int mu_tests_run = 0;

MU_TEST_SUITE(test_suite)
{
    /* hashing tests */
    MU_RUN_TEST(test_murmur3_x86_32);

    /* table cache tests */
#if defined(WITH_TABLE_CACHE)
    MU_RUN_TEST(cache_test_create_delete);
    MU_RUN_TEST(cache_test_allocator_stride);
    MU_RUN_TEST(cache_test_freelist_addressing);
#endif

    /* HAMT data structure tests */
    MU_RUN_TEST(test_aspell_dict_en);
    MU_RUN_TEST(test_popcount);
    MU_RUN_TEST(test_compact_index);
    MU_RUN_TEST(test_tagging);
    MU_RUN_TEST(test_search);
    MU_RUN_TEST(test_set_with_collisions);
    MU_RUN_TEST(test_set_whole_enchilada_00);
    MU_RUN_TEST(test_set_stringkeys);
    MU_RUN_TEST(test_setget_zero);
    MU_RUN_TEST(test_setget_large_scale);
    MU_RUN_TEST(test_shrink_table);
    MU_RUN_TEST(test_gather_table);
    MU_RUN_TEST(test_remove);
    MU_RUN_TEST(test_create_delete);
    MU_RUN_TEST(test_size);
    MU_RUN_TEST(test_iterators);
    MU_RUN_TEST(test_iterators_1m);
    // persistent data structure tests
    MU_RUN_TEST(test_persistent_set);
    MU_RUN_TEST(test_persistent_aspell_dict_en);
    MU_RUN_TEST(test_persistent_remove_aspell_dict_en);
    MU_RUN_TEST(test_table_extend);
    MU_RUN_TEST(test_persistent_setget_one);
    // tree statistics
    MU_RUN_TEST(test_tree_depth);
    return 0;
}

int main(void)
{
    printf("---=[ Hash array mapped trie tests\n");
    char *result = test_suite();
    if (result != 0) {
        printf("%s\n", result);
    } else {
        printf("All tests passed.\n");
    }
    printf("Tests run: %d\n", mu_tests_run);
    return result != 0;
}
