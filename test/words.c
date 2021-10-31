#include "words.h"

#include <stdio.h>
#include <string.h>

const size_t WORDS_MAX = 235886;

void words_load(char ***words, size_t n_words)
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

void words_free(char **words, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        free(words[i]);
    }
    free(words);
}

char **words_create_shuffled_refs(char **words, size_t n_words)
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

void words_free_refs(char **word_refs) { free(word_refs); }
