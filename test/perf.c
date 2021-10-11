#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "hamt.h"
#include "murmur3.h"

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

static void load_words(char ***words, size_t n_words)
{
    FILE *fp;
    char *line = NULL;
    size_t len = 0;
    ssize_t n, k;

    size_t i = 0;
    fp = fopen("test/words", "r");
    *words = calloc(n_words, sizeof(char *));
    while ((n = getline(&line, &len, fp)) != -1 && i < n_words) {
        k = line[n - 1] == '\n' ? n - 1 : n;
        (*words)[i] = strndup(line, k);
        line = NULL;
        ++i;
    }
    fclose(fp);
}

static void free_words(char **words, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        free(words[i]);
    }
    free(words);
}

static char **create_shuffled_words(char **words, size_t n_words)
{
    char **shuffled = calloc(n_words, sizeof(char *));
    memcpy(shuffled, words, n_words * sizeof(char *));
    char *tmp;
    for (size_t i = 0; i < n_words - 1; ++i) {
        size_t j = drand48() * (i + 1);
        tmp = shuffled[i];
        shuffled[i] = shuffled[j];
        shuffled[j] = tmp;
    }
    return shuffled;
}

static void delete_shuffled_words(char **words) { free(words); }

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
    enum { N = 235886 };
    char **words = NULL;
    char **shuffled = NULL;
    HAMT t;

    load_words(&words, N);
    struct TimeInterval ti_load;
    for (size_t i = 0; i < reps; ++i) {
        t = hamt_create(my_keyhash_string, my_keycmp_string);
        shuffled = create_shuffled_words(words, N);
        timer_start(&ti_load);
        for (size_t i = 0; i < N; i++) {
            hamt_set(t, words[i], words[i]);
        }
        timer_stop(&ti_load);
        hamt_delete(t);
        delete_shuffled_words(shuffled);
        print_timer(&ti_load, i, "load_table");
    }
    free_words(words, N);
}

static void perf_query_table(size_t reps)
{
    enum { N = 235886 };
    char **words = NULL;
    char **shuffled = NULL;
    HAMT t;

    load_words(&words, N);
    /* load table */
    t = hamt_create(my_keyhash_string, my_keycmp_string);
    for (size_t i = 0; i < N; i++) {
        hamt_set(t, words[i], words[i]);
    }

    struct TimeInterval ti_query;
    for (size_t i = 0; i < reps; ++i) {
        shuffled = create_shuffled_words(words, N);
        timer_start(&ti_query);
        for (size_t i = 0; i < N; i++) {
            hamt_get(t, words[i]);
        }
        timer_stop(&ti_query);
        delete_shuffled_words(shuffled);
        print_timer(&ti_query, i, "query_table");
    }
    /* cleanup */
    hamt_delete(t);
    free_words(words, N);
}

int main(int argc, char **argv)
{

    time_t now = time(0);
    srand(now);
    perf_load_table(25);
    perf_query_table(25);
    return 0;
}
