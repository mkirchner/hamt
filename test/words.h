#ifndef HAMT_TEST_WORDS_H
#define HAMT_TEST_WORDS_H

#include <stdlib.h>

/*
 * Utility constants and functions for the english word list in
 * text/words.txt.
 */

extern const size_t WORDS_MAX;

void words_load(char ***words, size_t n_words);
void words_free(char **words, size_t n);
/* Shuffle words randomly; this creates weak refs to the existing words */
char **words_create_shuffled_refs(char **words, size_t n_words);
void words_free_refs(char **word_refs);

#endif
