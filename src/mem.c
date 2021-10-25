#include "mem.h"

#include <assert.h>
#include <stdlib.h>

void *hamt_alloc(struct HamtAllocator *ator, size_t size)
{
    assert(ator != NULL);
    return ator->malloc(size);
}

void *hamt_realloc(struct HamtAllocator *ator, void *ptr, size_t size)
{
    assert(ator != NULL);
    return ator->realloc(ptr, size);
}

void hamt_free(struct HamtAllocator *ator, void *ptr)
{
    if (ptr)
        ator->free(ptr);
}

/* default allocator implementation */

static void *mem_alloc(size_t size) { return calloc(1, size); }

static void *mem_realloc(void *ptr, size_t size) { return realloc(ptr, size); }

static void mem_free(void *ptr) { free(ptr); }

struct HamtAllocator hamt_allocator_default = {mem_alloc, mem_realloc,
                                               mem_free};
