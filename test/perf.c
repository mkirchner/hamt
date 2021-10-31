#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "hamt.h"
#include "murmur3.h"
#include "words.h"

struct TimeInterval {
    struct timespec begin, end;
    time_t sec;
    long nsec;
};

void timer_start(struct TimeInterval *ti)
{
    ti->sec = 0;
    ti->nsec = 0;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ti->begin);
}

void timer_stop(struct TimeInterval *ti)
{
    clock_gettime(CLOCK_MONOTONIC_RAW, &ti->end);
    ti->sec += ti->end.tv_sec - ti->begin.tv_sec;
    ti->nsec += ti->end.tv_nsec - ti->begin.tv_nsec;
}

void timer_continue(struct TimeInterval *ti)
{
    clock_gettime(CLOCK_MONOTONIC_RAW, &ti->begin);
}

void print_timer(struct TimeInterval *ti, size_t ix, const char *tag)
{
    printf("%lu, %s,%lu\n", ix, tag, ti->sec * 1000000000L + ti->nsec);
}

static uint32_t my_keyhash_string(const void *key, const size_t gen)
{
    uint32_t hash = murmur3_32((uint8_t *)key, strlen((const char *)key), gen);
    return hash;
}

static int my_keycmp_string(const void *lhs, const void *rhs)
{
    /* expects lhs and rhs to be pointers to 0-terminated strings */
    size_t nl = strlen((const char *)lhs);
    size_t nr = strlen((const char *)rhs);
    return strncmp((const char *)lhs, (const char *)rhs, nl > nr ? nl : nr);
}

static void perf_load_table(size_t reps)
{
    char **words = NULL;
    char **shuffled = NULL;
    HAMT t;

    words_load(&words, WORDS_MAX);

    struct TimeInterval ti_load;
    for (size_t i = 0; i < reps; ++i) {
        t = hamt_create(my_keyhash_string, my_keycmp_string);
        shuffled = words_create_shuffled_refs(words, WORDS_MAX);
        timer_start(&ti_load);
        for (size_t i = 0; i < WORDS_MAX; i++) {
            hamt_set(t, words[i], words[i]);
        }
        timer_stop(&ti_load);
        hamt_delete(t);
        words_free_refs(shuffled);
        print_timer(&ti_load, i, "load_table");
    }

    words_free(words, WORDS_MAX);
}

static void perf_query_table(size_t reps)
{
    char **words = NULL;
    char **shuffled = NULL;
    HAMT t;

    words_load(&words, WORDS_MAX);

    /* load table */
    t = hamt_create(my_keyhash_string, my_keycmp_string);
    for (size_t i = 0; i < WORDS_MAX; i++) {
        hamt_set(t, words[i], words[i]);
    }

    struct TimeInterval ti_query;
    for (size_t i = 0; i < reps; ++i) {
        shuffled = words_create_shuffled_refs(words, WORDS_MAX);
        timer_start(&ti_query);
        for (size_t i = 0; i < WORDS_MAX; i++) {
            hamt_get(t, words[i]);
        }
        timer_stop(&ti_query);
        words_free_refs(shuffled);
        print_timer(&ti_query, i, "query_table");
    }
    /* cleanup */
    hamt_delete(t);

    words_free(words, WORDS_MAX);
}

int main(int argc, char **argv)
{

    time_t now = time(0);
    srand(now);
    perf_load_table(25);
    perf_query_table(25);
    return 0;
}
