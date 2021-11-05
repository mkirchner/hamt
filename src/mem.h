#ifndef MEM_H
#define MEM_H

#include <stddef.h>

void *mem_alloc(size_t size);
void *mem_realloc(void *ptr, size_t size);
void mem_free(void *ptr);

#endif /* MEM_H */
