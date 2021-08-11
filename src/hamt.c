#include "hamt.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Pointer tagging */
#define HAMT_TAG_MASK 0x3 /* last two bits */
#define HAMT_TAG_LEAF 0x1

#define tag(__p) (HamtNode *)((uintptr_t)__p | HAMT_TAG_LEAF)
#define untag(__p) (HamtNode *)((uintptr_t)__p & ~HAMT_TAG_MASK)
#define is_leaf(__p) (((uintptr_t)__p & HAMT_TAG_MASK) == HAMT_TAG_LEAF)

static int get_popcount(uint32_t n) { return __builtin_popcount(n); }

static int get_compact_index(uint32_t sparse_index, uint32_t bitmap)
{
    return get_popcount(bitmap & ((1 << sparse_index) - 1));
}

void *hamt_get(const HAMT *trie, uint32_t key) { return NULL; }

int hamt_set(HAMT *trie, uint32_t key, void *value) { return 0; }

void *hamt_delete(HAMT *trie, uint32_t key) { return NULL; }

static int to_dot_internal(const HamtINode *i, FILE *f)
{
    fprintf(f, "struct%p [label=\"{%p|{", i, i);
    for (size_t k = 31; k; --k) {
        if ((i->bitmap) & (1 << k)) {
            fprintf(f, "1");
        }
        if (k > 0)
            fprintf(f, "|");
    }
    fprintf(f, "|}|{");
    int n = get_popcount(i->bitmap);
    for (int k = (n - 1); k >= 0; --k) {
        fprintf(f, "<f%p>", untag(i->sub[k]));
        if (k > 0)
            fprintf(f, "|");
    }
    fprintf(f, "}}\"];\n");
    for (int k = (n - 1); k >= 0; --k) {
        fprintf(f, "struct%p:f%p:c -> struct%p\n", i, untag(i->sub[k]),
                untag(i->sub[k]));
    }
    return n;
}

static void to_dot_leaf(const HamtLNode *l, FILE *f)
{
    fprintf(f, "struct%p [label=\"{%p|{%p}}\"];\n", l, l, l->val);
}

static void to_dot_node(const HamtNode *node, FILE *f)
{
    if (!is_leaf(node)) {
        int popcount = to_dot_internal(&node->i, f);
        for (size_t k = 0; k < popcount; ++k) {
            to_dot_node(node->i.sub[k], f);
        }
    } else {
        to_dot_leaf(&(untag(node))->l, f);
    }
}

void hamt_to_dot(const HAMT *hamt, FILE *f)
{
    char *header = "digraph hamt {\n"
                   "  node [shape=record];\n"
                   "  edge [tailclip=false, arrowtail=dot, dir=both];\n";
    char *footer = "}";
    fprintf(f, "%s", header);
    to_dot_node(hamt->root, f);
    fprintf(f, "%s", footer);
}
