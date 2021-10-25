#ifndef MEM_H
#define MEM_H

#include <stddef.h>

struct HamtAllocator {
    void *(*malloc)(const size_t size);
    void *(*realloc)(void *chunk, const size_t size);
    void (*free)(void *chunk);
};

extern struct HamtAllocator hamt_allocator_default;

void *hamt_alloc(struct HamtAllocator *ator, size_t size);
void *hamt_realloc(struct HamtAllocator *ator, void *ptr, size_t size);
void hamt_free(struct HamtAllocator *ator, void *ptr);

#endif /* MEM_H */
