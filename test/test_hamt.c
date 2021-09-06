#include "minunit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "murmur3.h"
#include "utils.h"

#include "../src/hamt.c"

static char *test_popcount()
{
    printf(". testing popcount\n");
    /* we're relying on a built-in, spot-check a few cases */
    struct {
        uint32_t number;
        uint32_t nbits;
    } test_cases[4] = {{0, 0}, {42, 3}, {1337, 6}, {UINT32_MAX, 32}};

    for (size_t i = 0; i < 4; ++i) {
        mu_assert(get_popcount(test_cases[i].number) == test_cases[i].nbits,
                  "Unexpected number of set bits");
    }
    return 0;
}

static char *test_compact_index()
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
        mu_assert(get_pos(test_cases[i].sparse_index, bitmap) ==
                      test_cases[i].expected_dense_index,
                  "Unexpected dense index");
    }
    return 0;
}

static char *test_tagging()
{
    printf(". testing pointer tagging\n");
    HamtNode n;
    HamtNode *p = &n;
    mu_assert(!is_value(p), "Raw pointer must not be tagged");
    p = tagged(p);
    mu_assert(is_value(p),
              "Tagged pointer should be detected as tagged pointer");
    p = untagged(p);
    mu_assert(!is_value(p), "Untagging must return a raw pointer");
    return 0;
}

static char *test_murmur3_x86_32()
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
        mu_assert(hash == test_cases[i].expected, "Wrong hash");
    }
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

static char *test_search()
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
    char buf[38];
    for (size_t i = 0; i < 6; ++i) {
        uint32_t hash = my_hash_1(&keys[i], 0);
        printf("    %c -> %08x : %s [ ", keys[i], hash, i2b(hash, buf));
        print_keys(hash);
        printf("]\n");
    }

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

    HamtNode *t_8 = (HamtNode *)calloc(sizeof(HamtNode), 2);
    t_8[0].as.kv.key = &keys[2];
    t_8[0].as.kv.value = tagged(&values[2]);
    t_8[1].as.kv.key = &keys[3];
    t_8[1].as.kv.value = tagged(&values[3]);

    HamtNode *t_23 = (HamtNode *)calloc(sizeof(HamtNode), 2);
    t_23[0].as.kv.key = &keys[4];
    t_23[0].as.kv.value = tagged(&values[4]);
    t_23[1].as.kv.key = &keys[1];
    t_23[1].as.kv.value = tagged(&values[1]);

    HamtNode *t_root = (HamtNode *)calloc(sizeof(HamtNode), 3);
    t_root[0].as.table.index = (1 << 4) | (1 << 17);
    t_root[0].as.table.ptr = t_8;
    t_root[1].as.table.index = (1 << 0) | (1 << 16);
    t_root[1].as.table.ptr = t_23;
    t_root[2].as.kv.key = &keys[0];
    t_root[2].as.kv.value = tagged(&values[0]);

    HAMT t;
    t.key_cmp = my_strncmp_1;
    t.root->as.table.index = (1 << 8) | (1 << 23) | (1 << 31);
    t.root->as.table.ptr = t_root;

    struct {
        char *key;
        SearchStatus expected_status;
        int expected_value;
    } test_cases[10] = {
        {"0", SEARCH_SUCCESS, 0},       {"1", SEARCH_FAIL_NOTFOUND, 0},
        {"2", SEARCH_SUCCESS, 2},       {"3", SEARCH_FAIL_NOTFOUND, 0},
        {"4", SEARCH_SUCCESS, 4},       {"5", SEARCH_FAIL_NOTFOUND, 0},
        {"6", SEARCH_FAIL_NOTFOUND, 0}, {"7", SEARCH_SUCCESS, 7},
        {"8", SEARCH_SUCCESS, 8},       {"c", SEARCH_FAIL_KEYMISMATCH, 0}};

    for (size_t i = 0; i < 10; ++i) {
        Hash hash = {.key = test_cases[i].key,
                     .hash_fn = my_hash_1,
                     .hash = my_hash_1(test_cases[i].key, 0),
                     .depth = 0,
                     .shift = 0};
        SearchResult sr = search(t.root, hash, my_strncmp_1, test_cases[i].key);
        mu_assert(sr.status == test_cases[i].expected_status,
                  "Unexpected search result status");
        if (test_cases[i].expected_status == SEARCH_SUCCESS) {
            /* test key */
            mu_assert(0 == my_strncmp_1(test_cases[i].key,
                                        (char *)sr.value->as.kv.key),
                      "Successful search returns non-matching key");
            /* test value */
            mu_assert(test_cases[i].expected_value ==
                          *(int *)untagged(sr.value->as.kv.value),
                      "Successful search returns wrong value");
        }
    }

    free(t_root);
    free(t_23);
    free(t_8);
    return 0;
}

char *test_set_with_collisions()
{
    printf(". testing set/insert w/ forced key collision\n");
    HAMT *t = hamt_create(my_hash_1, my_strncmp_1);

    /* example 1: no hash collisions */
    char keys[] = "028";
    int values[] = {0, 2, 8};

    HamtNode *t_root = (HamtNode *)calloc(sizeof(HamtNode), 3);
    t_root[0].as.kv.key = &keys[0];
    t_root[0].as.kv.value = tagged(&values[0]);
    t_root[1].as.kv.key = &keys[1];
    t_root[1].as.kv.value = tagged(&values[1]);

    t->root->as.table.ptr = t_root;
    t->root->as.table.index = (1 << 23) | (1 << 31);

    /* insert value and find it again */
    const HamtNode *new_node =
        set(t->root, t->key_hash, t->key_cmp, &keys[2], &values[2]);
    Hash hash = {.key = &keys[2],
                 .hash_fn = t->key_hash,
                 .hash = t->key_hash(&keys[2], 0),
                 .depth = 0,
                 .shift = 0};
    SearchResult sr = search(t->root, hash, t->key_cmp, &keys[2]);
    mu_assert(sr.status == SEARCH_SUCCESS, "failed to find inserted value");
    mu_assert(new_node == sr.value, "Query result points to the wrong node");
    return 0;
}

char *test_set_whole_enchilada_00()
{
    printf(". testing set/insert w/ key collision\n");

    /* test data, see above */
    struct {
        char key;
        int value;
    } data[5] = {{'0', 0}, {'2', 2}, {'4', 4}, {'7', 7}, {'8', 8}};

    HAMT *t = hamt_create(my_hash_1, my_strncmp_1);
    for (size_t i = 0; i < 5; ++i) {
        // printf("setting (%c, %d)\n", data[i].key, data[i].value);
        set(t->root, t->key_hash, t->key_cmp, &data[i].key, &data[i].value);
        // debug_print(t->root, 4);
    }

    for (size_t i = 0; i < 5; ++i) {
        // printf("querying (%c, %d)\n", data[i].key, data[i].value);
        Hash hash = {.key = &data[i].key,
                     .hash_fn = t->key_hash,
                     .hash = t->key_hash(&data[i].key, 0),
                     .depth = 0,
                     .shift = 0};
        SearchResult sr = search(t->root, hash, t->key_cmp, &data[i].key);
        mu_assert(sr.status == SEARCH_SUCCESS, "failed to find inserted value");
        int *value = (int *)untagged(sr.value->as.kv.value);
        mu_assert(value, "found value is NULL");
        mu_assert(*value == data[i].value, "value mismatch");
        mu_assert(value == &data[i].value, "value pointer mismatch");
    }

    return 0;
}

static int my_keycmp_string(const void *lhs, const void *rhs)
{
    /* expects lhs and rhs to be pointers to 0-terminated strings */
    size_t nl = strlen((const char *)lhs);
    size_t nr = strlen((const char *)rhs);
    return strncmp((const char *)lhs, (const char *)rhs, nl > nr ? nl : nr);
}

const char *strconcat(const char *s1, const char *s2)
{
    size_t n1 = strlen(s1);
    size_t n2 = strlen(s2);
    char *str = malloc(n1 + n2 + 1);
    memcpy(str, s1, n1);
    memcpy(str + n1, s2, n2 + 1); // copy \0 from second string
    return str;
}

static uint32_t my_keyhash_string(const void *key, const size_t gen)
{
    uint32_t hash = murmur3_32((uint8_t *)key, strlen((const char *)key), gen);
    return hash;
}

char *test_set_stringkeys()
{
    printf(". testing set/insert w/ string keys\n");

    /* test data, see above */
    struct {
        char *key;
        int value;
    } data[6] = {{"humpty", 1}, {"dumpty", 2}, {"sat", 3},
                 {"on", 4},     {"the", 5},    {"wall", 6}};

    HAMT *t = hamt_create(my_keyhash_string, my_keycmp_string);
    for (size_t i = 0; i < 6; ++i) {
        // printf("setting (%s, %d)\n", data[i].key, data[i].value);
        set(t->root, t->key_hash, t->key_cmp, data[i].key, &data[i].value);
        // debug_print_string(t->root, 4);
    }

    for (size_t i = 0; i < 6; ++i) {
        // printf("querying (%s, %d)\n", data[i].key, data[i].value);
        Hash hash = {.key = data[i].key,
                     .hash_fn = t->key_hash,
                     .hash = t->key_hash(data[i].key, 0),
                     .depth = 0,
                     .shift = 0};
        SearchResult sr = search(t->root, hash, t->key_cmp, data[i].key);
        mu_assert(sr.status == SEARCH_SUCCESS, "failed to find inserted value");
        int *value = (int *)untagged(sr.value->as.kv.value);
        mu_assert(value, "found value is NULL");
        // printf("    %s: %d == %d\n", sr.value->as.kv.key, *value,
        // data[i].value);
        mu_assert(*value == data[i].value, "value mismatch");
        mu_assert(value == &data[i].value, "value pointer mismatch");
    }

    return 0;
}

char *test_aspell_dict_en()
{
    printf(". testing large-scale set/insert w/ string keys\n");

    FILE *fp;
    char *line = NULL;
    size_t len = 0;
    ssize_t n, k;
    char *tmp;

    fp = fopen("test/words", "r");
    mu_assert(fp, "Failed to open test dictionary");

    HAMT *t = hamt_create(my_keyhash_string, my_keycmp_string);

    while ((n = getline(&line, &len, fp)) != -1) {
        k = line[n - 1] == '\n' ? n - 1 : n;
        // FIXME: mem leak
        tmp = strndup(line, k);
        hamt_set(t, tmp, tmp);
        line = NULL;
    }

    /* Check if we can retrieve the entire dictionary */
    fseek(fp, 0, SEEK_SET);
    int i = 0;
    size_t maxdepth = 0;
    const char *value;
    while ((n = getline(&line, &len, fp)) != -1) {
        ++i;
        k = line[n - 1] == '\n' ? n - 1 : n;
        tmp = strndup(line, k);
        line = NULL;
        value = (const char *)hamt_get(t, tmp);
        mu_assert(value, "failed to retrieve existing value");
        mu_assert(strncmp(value, tmp, k) == 0, "invalid value");
    }
    fclose(fp);

    /* Check if "bluism" has search depth 7 */
    char target[] = "bluism";
    Hash hash = {.key = target,
                 .hash_fn = my_keyhash_string,
                 .hash = my_keyhash_string(target, 0),
                 .depth = 0,
                 .shift = 0};
    SearchResult sr = search(t->root, hash, t->key_cmp, target);
    mu_assert(sr.status == SEARCH_SUCCESS, "fail");
    value = (char *) untagged(sr.value->as.kv.value);
    mu_assert(value, "failed to retrieve existing value");
    mu_assert(strcmp(value, target) == 0, "invalid value");
    mu_assert(sr.hash.depth == 7, "invalid depth");
    return 0;
}

int tests_run = 0;

static char *test_suite()
{
    mu_run_test(test_popcount);
    mu_run_test(test_compact_index);
    mu_run_test(test_tagging);
    mu_run_test(test_murmur3_x86_32);
    mu_run_test(test_search);
    mu_run_test(test_set_with_collisions);
    mu_run_test(test_set_whole_enchilada_00);
    mu_run_test(test_set_stringkeys);
    mu_run_test(test_aspell_dict_en);
    // add more tests here
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
    printf("Tests run: %d\n", tests_run);
    return result != 0;
}
