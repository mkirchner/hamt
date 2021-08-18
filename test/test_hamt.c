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
        mu_assert(get_compact_index(test_cases[i].sparse_index, bitmap) ==
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

static int my_strncmp(const void *lhs, const void *rhs, size_t len)
{
    return strncmp((const char *)lhs, (const char *)rhs, len);
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
        uint32_t hash = murmur3_32((uint8_t *)&keys[i], 1, 0);
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
    t.cmp_eq = my_strncmp;
    t.seed = 0;
    t.size = 5;
    t.root.as.table.index = (1 << 8) | (1 << 23) | (1 << 31);
    t.root.as.table.ptr = t_root;

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
        uint32_t hash = murmur3_32((uint8_t *)test_cases[i].key, 1, 0);
        SearchResult sr =
            search(&t.root, hash, test_cases[i].key, 1, 0, my_strncmp);
        mu_assert(sr.status == test_cases[i].expected_status,
                  "Unexpected search result status");
        if (test_cases[i].expected_status == SEARCH_SUCCESS) {
            /* test key */
            mu_assert(0 == my_strncmp(test_cases[i].key,
                                      (char *)sr.value->as.kv.key, 1),
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

int tests_run = 0;

static char *test_suite()
{
    mu_run_test(test_popcount);
    mu_run_test(test_compact_index);
    mu_run_test(test_tagging);
    mu_run_test(test_murmur3_x86_32);
    mu_run_test(test_search);
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
