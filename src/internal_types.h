#ifndef HAMT_TYPES_INTERNAL_H
#define HAMT_TYPES_INTERNAL_H

#include <stdint.h>

/* HAMT node structure */

#define TABLE(a) a->as.table.ptr
#define INDEX(a) a->as.table.index
#define VALUE(a) a->as.kv.value
#define KEY(a) a->as.kv.key

struct hamt_node {
    union {
        struct {
            void *value; /* tagged pointer */
            void *key;
        } kv;
        struct {
            struct hamt_node *ptr;
            uint32_t index;
        } table;
    } as;
};
#endif
