#include "minunit.h"
#include <stdio.h>
#include <stdlib.h>

#include "../src/murmur3.c"


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

int mu_tests_run = 0;

MU_TEST_SUITE(test_suite)
{
    MU_RUN_TEST(test_murmur3_x86_32);
    return 0;
}

int main()
{
    printf("---=[ Murmur hash function test suite\n");
    char *result = test_suite();
    if (result != 0) {
        printf("%s\n", result);
    } else {
        printf("All tests passed.\n");
    }
    printf("Tests run: %d\n", mu_tests_run);
    return result != 0;
}
