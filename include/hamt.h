#ifndef HAMT_H
#define HAMT_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

struct HamtNode;

typedef struct HamtNode {
  union {
    struct {
      void *value;
      void *key;
    } kv;
    struct {
      struct HamtNode *ptr;
      uint32_t index;
    } table;
  } as;
} HamtNode;

typedef int (*HamtCmpEqFn)(const void* lhs, const void* rhs, size_t len);

typedef struct HAMT {
  HamtNode root;
  HamtCmpEqFn cmp_eq;
  uint32_t seed;
  size_t size;
} HAMT;


HAMT* hamt_create(HamtCmpEqFn cmp_eq);
void hamt_delete(HAMT*);

const void *hamt_get(const HAMT *trie, void *key, size_t keylen);
int hamt_set(HAMT *trie, void *key, size_t keylen, void *value, size_t len);
void *hamt_remove(HAMT *trie, void *key, size_t keylen);

#endif /* HAMT_H */
