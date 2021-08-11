#ifndef HAMT_H
#define HAMT_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

struct HamtNode;
typedef struct HamtNode HamtNode;

struct HamtINode {
  uint32_t bitmap;      /* sparse->dense bitmap */
  struct HamtNode **sub; /* dense array of children */
};
typedef struct HamtINode HamtINode;

struct HamtLNode {
  void *val; /* pointer to value */
};
typedef struct HamtLNode HamtLNode;

struct HamtNode {
  union {
    HamtINode i; /* internal */
    HamtLNode l; /* leaf */
  };
};

typedef struct HAMT {
  HamtNode *root;
  size_t size;
} HAMT;

void* hamt_get(const HAMT* trie, uint32_t key);
int hamt_set(HAMT* trie, uint32_t key, void* value);
void* hamt_delete(HAMT* trie, uint32_t key);
void hamt_to_dot(const HAMT* hamt, FILE* f);

#endif /* HAMT_H */
