#include "minunit.h"
#include <stdio.h>
#include <string.h>

#include "../src/hamt.c"

static char *test_something()
{
    // add test code here
    return 0;
}

int tests_run = 0;

static char *test_suite()
{
    mu_run_test(test_something);
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
