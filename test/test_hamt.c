#include "minunit.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "murmur3.h"
#include "utils.h"
#include "words.h"

#include "../src/hamt.c"

/*
 * Prints `node` and all its descendants in the HAMT.
 *
 * @param ix Index of `node` in its table (for illustratrion only)
 * @param node Pointer to the anchor node
 * @param depth Tree depth as a parameter
 */
static void debug_print_string(size_t ix, const hamt_node *node, size_t depth)
{
    /* print node*/
    if (!is_value(node->as.kv.value)) {
        printf("%*s +- (%lu): %s", (int)depth * 2, "", ix, "[ ");
        for (size_t i = 0; i < 32; ++i) {
            if (node->as.table.index & (1 << i)) {
                printf("%2lu(%i) ", i, get_pos(i, node->as.table.index));
            }
        }
        printf("%s", "]\n");
        /* print table */
        int n = get_popcount(node->as.table.index);
        for (int i = 0; i < n; ++i) {
            debug_print_string(i, &node->as.table.ptr[i], depth + 1);
        }
    } else {
        /* print value */
        printf("%*s +- (%lu): (%s, %i)\n", (int)depth * 2, "", ix,
               (char *)node->as.kv.key, *(int *)untagged(node->as.kv.value));
    }
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
    hamt_node n;
    hamt_node *p = &n;
    MU_ASSERT(!is_value(p), "Raw pointer must not be tagged");
    p = tagged(p);
    MU_ASSERT(is_value(p),
              "Tagged pointer should be detected as tagged pointer");
    p = untagged(p);
    MU_ASSERT(!is_value(p), "Untagging must return a raw pointer");
    return 0;
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

    hamt_node *t_8 = (hamt_node *)calloc(sizeof(hamt_node), 2);
    t_8[0].as.kv.key = &keys[2];
    t_8[0].as.kv.value = tagged(&values[2]);
    t_8[1].as.kv.key = &keys[3];
    t_8[1].as.kv.value = tagged(&values[3]);

    hamt_node *t_23 = (hamt_node *)calloc(sizeof(hamt_node), 2);
    t_23[0].as.kv.key = &keys[4];
    t_23[0].as.kv.value = tagged(&values[4]);
    t_23[1].as.kv.key = &keys[1];
    t_23[1].as.kv.value = tagged(&values[1]);

    hamt_node *t_root = (hamt_node *)calloc(sizeof(hamt_node), 3);
    t_root[0].as.table.index = (1 << 4) | (1 << 17);
    t_root[0].as.table.ptr = t_8;
    t_root[1].as.table.index = (1 << 0) | (1 << 16);
    t_root[1].as.table.ptr = t_23;
    t_root[2].as.kv.key = &keys[0];
    t_root[2].as.kv.value = tagged(&values[0]);

    struct hamt t;
    t.key_cmp = my_strncmp_1;
    t.ator = &hamt_allocator_default;
    t.root = mem_alloc(t.ator, sizeof(hamt_node));
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
        hash_state *hash =
            &(hash_state){.key = test_cases[i].key,
                          .hash_fn = my_hash_1,
                          .hash = my_hash_1(test_cases[i].key, 0),
                          .depth = 0,
                          .shift = 0};
        search_result sr = search_recursive(&t, t.root, hash, my_strncmp_1,
                                            test_cases[i].key, NULL);
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
    struct hamt *t =
        hamt_create(my_hash_1, my_strncmp_1, &hamt_allocator_default);

    /* example 1: no hash collisions */
    char keys[] = "028";
    int values[] = {0, 2, 8};

    hamt_node *t_root = (hamt_node *)calloc(sizeof(hamt_node), 3);
    t_root[0].as.kv.key = &keys[0];
    t_root[0].as.kv.value = tagged(&values[0]);
    t_root[1].as.kv.key = &keys[1];
    t_root[1].as.kv.value = tagged(&values[1]);

    t->root->as.table.ptr = t_root;
    t->root->as.table.index = (1 << 23) | (1 << 31);

    /* insert value and find it again */
    const hamt_node *new_node =
        set(t, t->root, t->key_hash, t->key_cmp, &keys[2], &values[2]);
    hash_state *hash = &(hash_state){.key = &keys[2],
                                     .hash_fn = t->key_hash,
                                     .hash = t->key_hash(&keys[2], 0),
                                     .depth = 0,
                                     .shift = 0};
    search_result sr =
        search_recursive(t, t->root, hash, t->key_cmp, &keys[2], NULL);
    MU_ASSERT(sr.status == SEARCH_SUCCESS, "failed to find inserted value");
    MU_ASSERT(new_node == sr.value, "Query result points to the wrong node");
    hamt_delete(t);
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

    struct hamt *t =
        hamt_create(my_hash_1, my_strncmp_1, &hamt_allocator_default);
    for (size_t i = 0; i < 5; ++i) {
        set(t, t->root, t->key_hash, t->key_cmp, &data[i].key, &data[i].value);
    }

    for (size_t i = 0; i < 5; ++i) {
        hash_state *hash = &(hash_state){.key = &data[i].key,
                                         .hash_fn = t->key_hash,
                                         .hash = t->key_hash(&data[i].key, 0),
                                         .depth = 0,
                                         .shift = 0};
        search_result sr =
            search_recursive(t, t->root, hash, t->key_cmp, &data[i].key, NULL);
        MU_ASSERT(sr.status == SEARCH_SUCCESS, "failed to find inserted value");
        int *value = (int *)untagged(sr.value->as.kv.value);
        MU_ASSERT(value, "found value is NULL");
        MU_ASSERT(*value == data[i].value, "value mismatch");
        MU_ASSERT(value == &data[i].value, "value pointer mismatch");
    }
    hamt_delete(t);
    return 0;
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

MU_TEST_CASE(test_set_stringkeys)
{
    printf(". testing set/insert w/ string keys\n");

    /* test data, see above */
    struct {
        char *key;
        int value;
    } data[6] = {{"humpty", 1}, {"dumpty", 2}, {"sat", 3},
                 {"on", 4},     {"the", 5},    {"wall", 6}};

    struct hamt *t = hamt_create(my_keyhash_string, my_keycmp_string,
                                 &hamt_allocator_default);
    for (size_t i = 0; i < 6; ++i) {
        // printf("setting (%s, %d)\n", data[i].key, data[i].value);
        set(t, t->root, t->key_hash, t->key_cmp, data[i].key, &data[i].value);
        // debug_print_string(t->root, 4);
    }

    for (size_t i = 0; i < 6; ++i) {
        // printf("querying (%s, %d)\n", data[i].key, data[i].value);
        hash_state *hash = &(hash_state){.key = data[i].key,
                                         .hash_fn = t->key_hash,
                                         .hash = t->key_hash(data[i].key, 0),
                                         .depth = 0,
                                         .shift = 0};
        search_result sr =
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
    return 0;
}

MU_TEST_CASE(test_aspell_dict_en)
{
    printf(". testing large-scale set/insert w/ string keys\n");

    char **words = NULL;
    struct hamt *t;

    words_load(&words, WORDS_MAX);
    t = hamt_create(my_keyhash_string, my_keycmp_string,
                    &hamt_allocator_default);
    for (size_t i = 0; i < WORDS_MAX; i++) {
        hamt_set(t, words[i], words[i]);
    }

    /* Check if we can retrieve the entire dictionary */
    for (size_t i = 0; i < WORDS_MAX; i++) {
        MU_ASSERT(hamt_get(t, words[i]) != NULL, "could not find expected key");
    }

    /* Check if "bluism" has search depth 6 */
    char target[] = "bluism";
    hash_state *hash = &(hash_state){.key = target,
                                     .hash_fn = my_keyhash_string,
                                     .hash = my_keyhash_string(target, 0),
                                     .depth = 0,
                                     .shift = 0};
    search_result sr =
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
    t = hamt_create(my_keyhash_string, my_keycmp_string,
                    &hamt_allocator_default);
    for (size_t i = 0; i < n_items; i++) {
        hamt_set(t, words[i], words[i]);
        MU_ASSERT(hamt_get(t, words[i]), "Failed to get key we just pushed");
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
    struct hamt *t = hamt_create(my_keyhash_string, my_keycmp_string,
                                 &hamt_allocator_default);

    /* create table w/ 5 entries and delete each position */
    hamt_node *a0;
    for (size_t delete_pos = 0; delete_pos < N; delete_pos++) {
        a0 = mem_alloc(t->ator, sizeof(hamt_node));
        memset(a0, 0, sizeof(hamt_node));
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
        mem_free(t->ator, a0);
    }
    hamt_delete(t);
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
    struct hamt *t = hamt_create(my_keyhash_string, my_keycmp_string,
                                 &hamt_allocator_default);

    hamt_node *a0 = mem_alloc(t->ator, sizeof(hamt_node));
    a0->as.table.index = 0;
    a0->as.table.ptr = table_allocate(t, N);
    for (size_t i = 0; i < N; ++i) {
        a0->as.table.index |= (1 << data[i].index);
        a0->as.table.ptr[i].as.kv.key = (void *)data[i].key;
        a0->as.table.ptr[i].as.kv.value = tagged(&data[i].value);
    }

    hamt_node *a1 = table_gather(t, a0, 0);

    MU_ASSERT(a1->as.kv.key == data[0].key, "wrong key in gather");
    MU_ASSERT(untagged(a1->as.kv.value) == (void *)&data[0].value,
              "wrong value in gather");
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

    struct hamt *t = hamt_create(my_keyhash_string, my_keycmp_string,
                                 &hamt_allocator_default);

    for (size_t k = 0; k < 3; ++k) {
        for (size_t i = 0; i < N; ++i) {
            set(t, t->root, t->key_hash, t->key_cmp, data[i].key,
                &data[i].value);
        }
        for (size_t i = 0; i < N; ++i) {
            hash_state *hash =
                &(hash_state){.key = data[i].key,
                              .hash_fn = t->key_hash,
                              .hash = t->key_hash(data[i].key, 0),
                              .depth = 0,
                              .shift = 0};
            path_result pr =
                rem(t, t->root, t->root, hash, t->key_cmp, data[i].key);
            MU_ASSERT(pr.rr.status == REMOVE_SUCCESS ||
                          pr.rr.status == REMOVE_GATHERED,
                      "failed to find inserted value");
            MU_ASSERT(*(int *)untagged(pr.rr.value) == data[i].value,
                      "wrong value in remove");
        }
    }
    hamt_delete(t);
    return 0;
}

MU_TEST_CASE(test_create_delete)
{
    printf(". testing create/delete cycle\n");
    struct hamt *t;
    t = hamt_create(my_keyhash_string, my_keycmp_string,
                    &hamt_allocator_default);
    hamt_delete(t);

    t = hamt_create(my_keyhash_string, my_keycmp_string,
                    &hamt_allocator_default);
    struct {
        char *key;
        int value;
    } data[6] = {{"humpty", 1}, {"dumpty", 2}, {"sat", 3},
                 {"on", 4},     {"the", 5},    {"wall", 6}};
    for (size_t i = 0; i < 6; ++i) {
        set(t, t->root, t->key_hash, t->key_cmp, data[i].key, &data[i].value);
    }
    hamt_delete(t);
    return 0;
}

MU_TEST_CASE(test_size)
{
    printf(". testing tree size tracking\n");
    struct hamt *t;
    t = hamt_create(my_keyhash_string, my_keycmp_string,
                    &hamt_allocator_default);
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

    t = hamt_create(my_keyhash_string, my_keycmp_string,
                    &hamt_allocator_default);

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
    struct hamt *t;
    t = hamt_create(my_keyhash_string, my_keycmp_string,
                    &hamt_allocator_default);
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

    const struct hamt *t = hamt_create(my_keyhash_string, my_keycmp_string,
                                 &hamt_allocator_default);
    const struct hamt *tmp = t;
    for (size_t i = 0; i < 6; ++i) {
        tmp = hamt_pset(t, data[i].key, &data[i].value);
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
    t = hamt_create(my_keyhash_string, my_keycmp_string,
                    &hamt_allocator_default);
    for (size_t i = 0; i < WORDS_MAX; i++) {
        /* structural sharing */
        t = hamt_pset(t, words[i], words[i]);
    }

    /* Check if we can retrieve the entire dictionary */
    for (size_t i = 0; i < WORDS_MAX; i++) {
        MU_ASSERT(hamt_get(t, words[i]) != NULL, "could not find expected key");
    }

    /* Check if "bluism" has search depth 6 */
    char target[] = "bluism";
    hash_state *hash = &(hash_state){.key = target,
                                     .hash_fn = my_keyhash_string,
                                     .hash = my_keyhash_string(target, 0),
                                     .depth = 0,
                                     .shift = 0};
    search_result sr =
        search_recursive(t, t->root, hash, t->key_cmp, target, NULL);
    MU_ASSERT(sr.status == SEARCH_SUCCESS, "fail");
    char *value = (char *)untagged(sr.value->as.kv.value);

    MU_ASSERT(value, "failed to retrieve existing value");
    MU_ASSERT(strcmp(value, target) == 0, "invalid value");
    MU_ASSERT(sr.hash->depth == 6, "invalid depth");

    words_free(words, WORDS_MAX);
    /* There is no way to cleanly free the structurally shared
     * tries without garbage collection. Leak them. */
    return 0;
}

MU_TEST_CASE(test_persistent_remove_aspell_dict_en)
{
    printf(". testing large-scale remove w/ structural sharing\n");

    char **words = NULL;
    const struct hamt *t;

    words_load(&words, WORDS_MAX);
    t = hamt_create(my_keyhash_string, my_keycmp_string,
                    &hamt_allocator_default);
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
        MU_ASSERT(hamt_get(t, words[i]) != NULL,
                  "key should not have been removed from original trie");
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
    printf(". testing tree depth linearity assumptions");

    size_t n_items = 1e6;
    char **words = NULL;
    struct hamt *t;

    words_load_numbers(&words, 0, n_items);

    t = hamt_create(my_keyhash_string, my_keycmp_string,
                    &hamt_allocator_default);
    for (size_t i = 0; i < n_items; i++) {
        hamt_set(t, words[i], words[i]);
    }

    /* Calculate the avg tree depth across all items */
    double avg_depth = 0.0;
    size_t max_depth = 0;
    for (size_t i = 0; i < n_items; i++) {
        hash_state *hash = &(hash_state){.key = words[i],
                                         .hash_fn = my_keyhash_string,
                                         .hash = my_keyhash_string(words[i], 0),
                                         .depth = 0,
                                         .shift = 0};
        search_result sr =
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
        }
    }

    hamt_delete(t);
    words_free(words, n_items);
    printf(" (avg tree depth w/ %lu items: %f, expected %f, max: %lu)\n",
           n_items, avg_depth, log2(n_items) / 5.0,
           max_depth); /* log_32(n_items) */
    return 0;
}
int mu_tests_run = 0;

MU_TEST_SUITE(test_suite)
{
    MU_RUN_TEST(test_popcount);
    MU_RUN_TEST(test_compact_index);
    MU_RUN_TEST(test_tagging);
    MU_RUN_TEST(test_search);
    MU_RUN_TEST(test_set_with_collisions);
    MU_RUN_TEST(test_set_whole_enchilada_00);
    MU_RUN_TEST(test_set_stringkeys);
    MU_RUN_TEST(test_aspell_dict_en);
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
    MU_RUN_TEST(test_tree_depth);
    return 0;
}

int main()
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
