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
    mu_assert(!is_leaf(p), "Raw pointer must not be tagged");
    p = tag(p);
    mu_assert(is_leaf(p),
              "Tagged pointer should be detected as tagged pointer");
    p = untag(p);
    mu_assert(!is_leaf(p), "Untagging must return a raw pointer");
    return 0;
}

static char *test_murmur3_x86_32()
{
    printf(". testing Murmur3 (x86, 32bit)\n");
    /* test vectors from https://stackoverflow.com/questions/14747343/murmurhash3-test-vectors */
    struct {
        char* key;
        size_t len;
        uint32_t seed;
        uint32_t expected;
    } test_cases[7] = {
        { NULL, 0, 0, 0},
        { NULL, 0, 1, 0x514e28b7},
        { NULL, 0, 0xffffffff, 0x81f16f39},
        { "\x00\x00\x00\x00", 4, 0, 0x2362f9de},
        { "\xff\xff\xff\xff", 4, 0, 0x76293b50},
        { "\x21\x43\x65\x87", 4, 0, 0xf55b516b},
        { "\x21\x43\x65\x87", 4, 0x5082edee, 0x2362f9de},
    };

    for (size_t i = 0; i < 7; ++i) {
        uint32_t hash = murmur3_32((uint8_t*) test_cases[i].key,
                test_cases[i].len,
                test_cases[i].seed);
        mu_assert(hash == test_cases[i].expected, "Wrong hash");
    }
    return 0;
}

static char *test_dot()
{
    int vals[3] = {21, 42, 84};
    HamtNode* leaf21 = (HamtNode*) calloc(sizeof(HamtNode), 1);
    leaf21->l.val = &vals[0];
    HamtNode* leaf42 = (HamtNode*) calloc(sizeof(HamtNode), 1);
    leaf42->l.val = &vals[1];
    HamtNode* leaf84 = (HamtNode*) calloc(sizeof(HamtNode), 1);
    leaf84->l.val = &vals[2];

    HamtNode* internal1 = (HamtNode*) calloc(sizeof(HamtNode), 1);
    internal1->i.bitmap = (1 << 16 | 1 << 27);
    internal1->i.sub = calloc(sizeof(HamtNode*), 2);
    internal1->i.sub[0] = tag(leaf21);
    internal1->i.sub[1] = tag(leaf42);

    HamtNode* internal0 = (HamtNode*) calloc(sizeof(HamtNode), 1);
    internal0->i.bitmap = (1 << 16 | 1 << 8);
    internal0->i.sub = calloc(sizeof(HamtNode*), 2);
    internal0->i.sub[0] = internal1;
    internal0->i.sub[1] = tag(leaf84);

    HAMT* hamt = (HAMT*) malloc(sizeof(HAMT));
    hamt->root = internal0;

    FILE* f = fopen("test_dot.dot", "w");
    hamt_to_dot(hamt, f);
    fclose(f);
    free(leaf21);
    free(leaf42);
    free(internal1);
    free(internal0);
    free(hamt);
    return 0;
}

int tests_run = 0;

static char *test_suite()
{
    mu_run_test(test_popcount);
    mu_run_test(test_compact_index);
    mu_run_test(test_tagging);
    mu_run_test(test_murmur3_x86_32);
    // mu_run_test(test_dot);
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
        printf("All tests passed");
    }
    printf("Tests run: %d\n", tests_run);
    return result != 0;
}
