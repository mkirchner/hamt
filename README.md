# libhamt
A hash array-mapped trie (HAMT) implementation in C99. The implementation
follows Bagwell's 2000 paper[[1]][bagwell_00_ideal], with a focus on clarity
rather than raw speed.

The original motivation for this effort was the desire to understand and
implement an efficient persistent data structure with structural sharing for
maps and sets for [my own Lisp implementation][stutter].

What prompted the somewhat detailed writeup was the realization that there is
not a lot of in-depth documentation for HAMTs beyond the original Bagwell
paper[[1][bagwell_00_ideal]] Some of the more helpful posts are [Karl Krukow's
intro to Clojure's `PersistentHashMap`][krukov_09_understanding], [C. S. Lim's
C++ template implementation][chaelim_hamt], and [Adrian Coyler's morning paper
post][coyler_15_champ] on compressed HAMTs. There is more, but it's all in bits
and pieces. This is an attempt to (partially) improve that situation.

## Quickstart

To build the library and run the tests:

```bash
$ git clone git@github.com:mkirchner/hamt.git
$ cd hamt
$ make
$ make test
```

In order to use `libhamt` in your own projects, copy `include/hamt.h` and
`src/hamt.c` in your own source tree and build from there.

## Table of Contents

* [Introduction](#introduction)
* [API](#api)
   * [HAMT lifecycle](#hamt-lifecycle)
      * [Memory management](#memory-management)
   * [Query](#query)
      * [Iterators](#iterators)
   * [Modification: Insertion &amp; Removal](#modification-insertion--removal)
   * [Using the HAMT as an efficient persistent data structure](#using-the-hamt-as-an-efficient-persistent-data-structure)
   * [Examples](#examples)
      * [Example 1: ephemeral HAMT w/ standard allocation](#example-1-ephemeral-hamt-w-standard-allocation)
      * [Example 2: Changes required for garbage collection and persistence](#example-2-changes-required-for-garbage-collection-and-persistence)
      * [Example 3: Using iterators](#example-3-using-iterators)
* [Implementation](#implementation)
   * [Setup](#setup)
      * [Project structure](#project-structure)
      * [Building the project](#building-the-project)
   * [Design](#design)
      * [Foundational data structures](#foundational-data-structures)
   * [Hashing](#hashing)
      * [Hash exhaustion: hash generations and state management](#hash-exhaustion-hash-generations-and-state-management)
   * [Table management](#table-management)
   * [Putting it all together](#putting-it-all-together)
      * [Search](#search)
      * [Insert](#insert)
      * [Remove](#remove)
      * [Iterators](#iterators-1)
   * [Persistent data structures and structural sharing](#persistent-data-structures-and-structural-sharing)
      * [Basic idea: path copying](#basic-idea-path-copying)
      * [Insert](#insert-1)
      * [Remove](#remove-1)
* [Appendix](#appendix)
   * [Unit testing](#unit-testing)
* [Footnotes](#footnotes)

# Introduction

A *hash array mapped trie (HAMT)* is a data structure that can be used to
implement [associative arrays][wiki_associative_array] (aka maps) and
[sets][wiki_set_adt].

Structurally, HAMTs are [hash trees][wiki_hash_tree] that combine favorable
characteristics of [hash tables][wiki_hash_table] and array mapped
[tries][wiki_trie], namely almost hash table-like time complexity
guarantees[[1]][bagwell_00_ideal] (O(log<sub>32</sub>n)) and economic use of memory.

An additional benefit, and a key motivation for the work presented here, is that
augmentation of HAMTs with path copying and garbage collection allows for a
straightforward and efficient implementation of [persistent][wiki_persistent]
versions of maps and sets.

The remaining documentation starts with a description of the `libhamt` API and
two examples that demonstrate the use of a HAMT as an ephemeral and persistent
data structure, respectively. I then detail the implementation: starting from
the foundational data structures and the helper code required for hash
exhaustion and table management, we cover search, insertion, removal, and
iterators. The final implementation section introduces path copying and explains
the changes required to support persistent insert and remove operations. We
close with an outlook and an appendix.

# API

## HAMT lifecycle

The core data type exported in the `libhamt` interface is `HAMT`. In order to
create a `HAMT` instance, one must call `hamt_create()`, which requires a
hash function of type `HamtKeyHashFn` to hash keys, a comparison function of
type `HamtCmpFn` to compare keys, and a pointer to a `HamtAllocator` instance.
`hamt_delete()` deletes `HAMT` instances that were created with `hamt_create()`.


```c
typedef struct HamtImpl *HAMT;
typedef int (*HamtCmpFn)(const void *lhs, const void *rhs);
typedef uint32_t (*HamtKeyHashFn)(const void *key, const size_t gen);


HAMT hamt_create(HamtKeyHashFn key_hash, HamtCmpFn key_cmp,
                 struct HamtAllocator *ator);
void hamt_delete(HAMT);
```

The `HamtKeyHashFn` takes a `key` and a generation `gen`. The expectation is
that the supplied hash function returns different hashes for the same key but
different generations. Depending on the choice of hash function this can be
implemented using `gen` as a seed or modifying a copy of `key` on the fly.
See the [examples](#examples) section for a `murmur3`-based implementation and
the [hashing](#hashing) section for more information on suitable hash functions.


### Memory management

`libhamt` exports its internal memory management API through the `HamtAllocator`
struct. The struct specifies the functions that the HAMT implementation uses to
allocate, re-allocate and deallocate system memory. The API provides a default
`hamt_allocator_default` which refers to the standard `malloc()`, `realloc()`
and `free()` functions.

```c
struct HamtAllocator {
    void *(*malloc)(const size_t size);
    void *(*realloc)(void *chunk, const size_t size);
    void (*free)(void *chunk);
};

extern struct HamtAllocator hamt_allocator_default;
```

Exporting the `libhamt` memory management API enables library clients to make
use of alternate memory management solutions, most notably of garbage collection
solutions (e.g. the [Boehm-Demers-Weiser GC][boehm_gc]) which are required when
using the HAMT as a persistent data structure (see the [structural sharing
example](#example-2-garbage-collected-persistent-hamts)).


## Query

```c
size_t hamt_size(const HAMT trie);
const void *hamt_get(const HAMT trie, void *key);
```

The `hamt_size()` function returns the size of the HAMT in O(1). Querying the
HAMT (i.e. searching a key) is done with `hamt_get()` which takes a pointer to a
key and returns a result in O(log<sub>32</sub> n) - or `NULL` if the key does
not exist in the HAMT.

### Iterators

The API also provides key/value pair access through the `HamtIterator` struct.
```c
size_t hamt_size(const HAMT trie);
const void *hamt_get(const HAMT trie, void *key);
```

Iterators are tied to a specific HAMT and are created using the
`hamt_it_create()` function, passing the HAMT instance the iterator should refer
to. Iterators can be advanced with the `hamt_it_next()` function and as long as
`hamt_it_valid()` returns `true`, the `hamt_it_get_key()` and
`hamt_it_get_value()` functions will return the pointers to the current
key/value pair. In order to delete an existing and/or exhausted iterator, call
`hamt_it_delete()`.

```c
typedef struct HamtIteratorImpl *HamtIterator;

HamtIterator hamt_it_create(const HAMT trie);
void hamt_it_delete(HamtIterator it);
bool hamt_it_valid(HamtIterator it);
HamtIterator hamt_it_next(HamtIterator it);
const void *hamt_it_get_key(HamtIterator it);
const void *hamt_it_get_value(HamtIterator it);
```

Note that iterators maintain state about their traversal path and that changes
to the underlying HAMT the iterator refers to, will likely cause undefined
behavior.


## Insert & Remove

`libhamt` supports ephemeral and
[persistent][wiki_persistent] (aka not ephemeral) HAMTs through two different interfaces:
`hamt_set()` and `hamt_remove()` for ephemeral use, and their `p`-versions
`hamt_pset()` and `hamt_premove()` for persistent use.

### Ephemeral modification

```c
const void *hamt_set(HAMT trie, void *key, void *value);
void *hamt_remove(HAMT trie, void *key);
```

`hamt_set()` takes a pair of `key` and `value` pointers and adds the pair to the HAMT,
returning a pointer to the `value`. If the `key` already exists, `hamt_set()`
updates the pointer to the `value`.

`hamt_remove()` takes a `key` and removes the key/value pair with the
respective `key` from the HAMT, returning a pointer to the `value` that was
just removed. If the `key` does not exist, `hamt_remove()` returns `NULL`.

### Persistent HAMTs

The semantics of persistent HAMTs are different from their ephemeral
counterparts: since every modification creates a new version of a HAMT, the
modificiation functions return a new HAMT. Modification of a persistent HAMT
therefore requires a reassignment idiom if the goal is modification only:

```c
HAMT h = hamt_create(...)
...
/* Set a value and drop the reference to the old HAMT; the GC
 * will take care of cleaning up remaining unreachable allocations.
 */
h = hamt_pset(h, some_key, some_value);
...
```

This seems wasteful at first glance but the respective functions implement structural
sharing such that the overhead is limited to *~log<sub>32</sub>(N)* nodes (where *N* is the
number of nodes in the graph).

```c
const HAMT hamt_pset(const HAMT trie, void *key, void *value);
const HAMT hamt_premove(const HAMT trie, void *key);
```

`hamt_pset()` inserts or updates the `key` with `value` and returns an opaque
handle to the new HAMT. The new HAMT is guaranteed to contain the new
key/value pair.

`hamt_premove()` attempts to remove the value with the key `key`. It is *not*
an error if the key does not exist; the new HAMT is guaranteed to not contain
the key `key`.

## Examples

### Example 1: ephemeral HAMT w/ standard allocation

```c
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hamt.h"
#include "murmur3.h"


static uint32_t hash_string(const void *key, const size_t gen)
{
    return murmur3_32((uint8_t *)key, strlen((const char *)key), gen);
}

int main(int argn, char *argv[])
{
    enum { N = 5; };
    struct {
        char *country;
        char *capital;
    } cities[N] = {
        {"Germany", "Berlin"},
        {"Spain", "Madrid"},
        {"Italy", "Rome"},
        {"France", "Paris"},
        {"Romania", "Bucharest"}
        /* ... */
    };

    HAMT t;

    /* create table */
    t = hamt_create(hash_string, strcmp, &hamt_allocator_default);
    /* load table */
    for (size_t i = 0; i < N; i++) {
        hamt_set(t, cities[i].country, cities[i].capital);
    }

    /* query table */
    for (size_t i = 0; i < N; i++) {
        printf("%s has capital %s\n", cities[i].country,
                                      hamt_get(t, cities[i].country));
    }
    /* cleanup */
    hamt_delete(t);
    return 0;
}
```

### Example 2: Garbage-collected persistent HAMTs

The key to making use of structural sharing is to provide `libhamt` with a
`struct HamtAllocator` instance that implements garbage collection.

The example below uses the the [Boehm-Demers-Weiser][boehm_gc] GC. For
GC installation, compilation and linking instructions, please refer to the GC
documentation.

In brief, the Boehm GC provides a `gc.h` include file and drop-in replacements
for the standard memory management functions, including `malloc`, `realloc`
and `free`.

The following snippet illustrates the required changes:

```c
...
#include "gc.h"  /* Boehm-Demers-Weiser GC */

...

int main(int argc, char *argv[]) {
    ...
    /*
    Set up garbage collection. We set the function pointer for `free` to
    NULL to avoid explicit freeing of memory.
    */
    struct HamtAllocator gc_alloc = {GC_malloc, GC_realloc, NULL};
    t = hamt_create(hash_string, strcmp, &gc_alloc);
    ...
}
```

Note that we set the `gc_alloc.free` function pointer to `NULL` and not to `GC_free`
which would be the drop-in replacement provided by the garbage collection.
This ensures that we actually rely on the garbage collector and refrain from
explicit deletion.

### Example 3: Iterators

The following snipped illustrates how to create, test, exhaust and dispose of
an iterator. We first create the iterator using `hamt_it_create()`, jump into
a `while` loop and advance the iterator using `hamt_it_next()` while the
iterator is valid. In every interation we print the current key/value pair to
`stdout`. Once we exit the loop, we clean up using `hamt_it_delete()`.

```c
    ...
    HAMT t = hamt_create(hash_string, strcmp, &hamt_allocator_default);

    /* load table */
    ...

    /* create iterator */
    HamtIterator it = hamt_it_create(t);
    while (hamt_it_valid(it)) {
        printf("(%s, %s)\n", (char *)hamt_it_get_key(it),
                             (char *)hamt_it_get_value(it));
        hamt_it_next(it);
    }
    /* clean up */
    hamt_it_delete(it);

    ...
    hamt_delete(t);
    ...
```

This concludes the description of the `libhamt` interface and we now move on
to detailed implementation notes.

# Implementation

## Prelude: Setup

### Project structure

The `hamt` source tree has the following structure:

```
hamt/
  build/         Out-of-source build destination
  include/       Header files that are part of the interface
  src/           Source and header files
  test/          Test and utility headers & sources
  Makefile
```

Sources are organized in three folders: the `include` folder, for all header
files that are part of the public interface; the `src` folder, for the
actual implementation and private header files; and the `test` folder, for all
test code, including headers and sources for testing utilities (e.g. data
loading and benchmarking functions).

The build process is governed by a single `Makefile` in the project root
directory.

### Building the project

To build the library and run the tests:

```
$ make && make test
```

And, optionally, to run the performance tests:

```
$ make perf
```

The latter requires a somewhat current Python 3 installation with
`matplotlib` and `pandas` packages for graphing.

## Design

### Introduction

A hash array mapped trie forms an *n*-ary tree.  Internal and leaf nodes have
different types: internal nodes point to *n* internal or leaf nodes and leaf
nodes hold or point to data (i.e. the keys/value pairs).

The tree itself is a *hash tree*: it uses the *hash* of the key interpreted as
a sequence of bits, to detetermine the location of the leaf node that stores
the key/value pair.  This overcomes one of the potential drawbacks of tries,
namely that they grow in depth linearly with the length of the input.  Hash
tries partially remedy that situation: they use a hash function to pre-process
the value to be stored in the tree and use the bits of the hash to determine
the location of a particular value in the tree. The number of bits used at
every tree depth determines the fan out factor and the eventual depth of the
tree.

HAMTs implement *array mapping*: instead of reserving space for *n*
pointers to children in each internal node, the parent node stores a bitmap
that indicates which children are present and the actual node only allocates
the memory required to refer to its children. This is an important optimization
for graphs with a high branching factor (e.g. *n*=32) that simultaneously makes the
data structure more memory efficient and cache-friendly.

In the following we will address these three concepts in turn: we first define
the foundational data structure used to build a tree and introduce the concept
of an *anchor*. We then dive into hash functions and hash state management
required to make hashing work for trees of arbitrary depths and in the presence
of hash collisions. Lastly, we turn to *table management*, introducing a set
of functions used to create, modify, query and dispose of mapped arrays.

With all these pieces in place, we can then finally implement the insert/update,
query, and delete functions for non-persistent HAMTs. We will then introduce the
concept of path copying and close with the implementation of persistent
insert/update and delete functions for HAMTs.

### Foundational data structures

`libhamt` implements internal and leaf nodes in two different types called
`struct table` and `struct kv` respectively. Leaf nodes are straightforward:
```c
struct {
    void *value;
    void *key;
} kv;
```
They point to a `key` and a `value` and since C does not afford us with
templating we make use of `void*` pointers to support arbitrary data types
(foregoing other, potentially more type-safe solutions that make heavy use of
the C preprocessor).

Internal nodes are a little more complicated: they will need to hold a pointer
`ptr` that can point to two different types of of nodes, either `struct kv *`
or `struct table *` and an `index` that keeps track of branch occupancy for
`ptr`.  The standard C way to enable this is to bring `struct kv` and `struct
table` into a `union`:

```c
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
```
This way, `HamtNode` is (and `as.table.ptr` points to) a union that can be
interpreted as either an internal or a leaf node. The question remains how to
distinguish between node types in a way that allows us to create arrays of
mixed type? One option would be to add an `enum` field as part of `HamtNode`
that specifies the type. While possible, there is a more memory-efficient
solution: pointer tagging.

**Pointer tagging**. Since pointers need to be word-aligned, that leaves the
lower 3 bits of all pointers on 64-bit architectures set to zero. If we
carefully mask the actual pointer values when they are used, we can make use of
these bits to encode the data type.

Note the order of the pairs: since index is not a pointer and the bit-fiddling
constrains do not apply, we need to make sure it does not overlap w/ the tagged
pointer. Since the pointer to the key is used more often, we opt to tag the
value pointer.

<p align="center">
<img src="doc/img/hamt-overview.png" width="600"></img>
</p>
<p class="image-caption"><b>Figure 1:</b> HAMT data structure.
<code>libhamt</code> implements
HAMTs using linked, heap-allocated tables. Table rows hold
either an index vector and pointer to a subtable or pointers to key and
value (one pair of key/value pointers illustrated in blue, and implicit to all
empty table fields).</p>



* PS: avoiding mixed type, adding another bitmap mapping and keeping two
  arrays is a key change in LAMP (double-check this)


### The Anchor

* Anchor view

## Hashing

* what is a hash function?
* different classes: cryptographically secure, just efficient
It is a one-way function, that is, a function for which it is practically infeasible to invert or reverse the computation.


```c
typedef uint32_t (*HamtKeyHashFn)(const void *key, const size_t gen);
```

* hash functions schould be fast and show good distribution
* cryptographical security is not an issue
* examples: Knuth's universal hash, djb2, murmur
* we're choosing murmur3: simple implementation, great properties


```c
#ifndef MURMUR3_H
#define MURMUR3_H

#include <stdint.h>
#include <stdlib.h>

uint32_t murmur3_32(const uint8_t *key, size_t len, uint32_t seed);

#endif
```

This declares the *murmur* hash function. In its standard form `murmur3_32`
takes a pointer `key` to byte-sized objects, a count of `len` that speficies
the number of bytes to hash and a random seed `seed`.

Its [definition][murmur3] is concise:

```c
#include "murmur3.h"

#include <string.h>

static inline uint32_t murmur_32_scramble(uint32_t k)
{
    k *= 0xcc9e2d51;
    k = (k << 15) | (k >> 17);
    k *= 0x1b873593;
    return k;
}

uint32_t murmur3_32(const uint8_t *key, size_t len, uint32_t seed)
{
    uint32_t h = seed;
    uint32_t k;
    /* Read in groups of 4. */
    for (size_t i = len >> 2; i; i--) {
        memcpy(&k, key, sizeof(uint32_t));
        key += sizeof(uint32_t);
        h ^= murmur_32_scramble(k);
        h = (h << 13) | (h >> 19);
        h = h * 5 + 0xe6546b64;
    }
    /* Read the rest. */
    k = 0;
    for (size_t i = len & 3; i; i--) {
        k <<= 8;
        k |= key[i - 1];
    }
    h ^= murmur_32_scramble(k);
    /* Finalize. */
    h ^= len;
    h ^= h >> 16
    h *= 0x85ebca6b;
    h ^= h >> 13;
    h *= 0xc2b2ae35;
    h ^= h >> 16;
    return h;
}
```

[Add some info about murmur3 here]

hamt has unit tests that validate the murmur hash results against know
values (add link here)

In order to use murmur3 as a `hamt` hash function, we need to wrap it into a
helper function:

```c
static uint32_t my_keyhash_string(const void *key, const size_t gen)
{
    uint32_t hash = murmur3_32((uint8_t *)key, strlen((const char *)key), gen);
    return hash;
}
```

Here, the wrapper makes use of `strlen(3)`, assuming valid C strings as keys.
Note the use of `gen` as a seed for the hash.


### Hash exhaustion: hash generations and state management

For a hash trie, the number of elements in the trie is limited by the total number
of hashes that fits into a 32-bit `uint32_t`, i.e. 2^32-1. Since the HAMT only
uses 30 bits (in 6 chunks of 5 bits), the number of unique keys in the trie is
limited to 2^30-1 = 1,073,741,823 keys. 
At the same time, since every layer of the
tree uses 5 bits of the hash, the trie depth is limited to 6 layers.
Neither the hard limit to the number of elements in the trie,
nor the inability to build a trie beyond depth of 6 are desirable properties.

To address both issues, `libhamt` recalculates the hash with a different seed every
32/5 = 6 layers. This requires a bit of state management and motivates the
existence of the `Hash` data type and functions that operate on it:

```c
typedef struct Hash {
    const void *key;
    HamtKeyHashFn hash_fn;
    uint32_t hash;
    size_t depth;
    size_t shift;
} Hash;
```
The struct maintains the pointers `key` to the key that is being hashed and
`hash_fn` to the hash function used to calculate the current hash `hash`. At
the same time, it tracks the current depth `depth` in the tree (this is the
*hash generation*) and the bitshift `shift` of the current 5-bit hash chunk.

The interface provides two functions: the means to step from the current 5-bit
hash to the next in `hash_next()`; and the ability query the current index of a
key at the current trie depth in `hash_get_index()`.

```c
static inline Hash *hash_next(Hash *h)
{
    h->depth += 1;
    h->shift += 5;
    if (h->shift > 30) {
        h->hash = h->hash_fn(h->key, h->depth / 5);
        h->shift = 0;
    }
    return h;
}
```

```c
static inline uint32_t hash_get_index(const Hash *h)
{
    return (h->hash >> h->shift) & 0x1f;
}
```


## Table management


```c
HamtNode *table_allocate(struct HamtAllocator *ator, size_t size)
{
    return (HamtNode *)mem_alloc(ator, (size * sizeof(HamtNode)));
}
```

```c
void table_free(struct HamtAllocator *ator, HamtNode *ptr, size_t size)
{
    mem_free(ator, ptr);
}

```

```c
HamtNode *table_extend(struct HamtAllocator *ator, HamtNode *anchor,
                       size_t n_rows, uint32_t index, uint32_t pos)
{
    HamtNode *new_table = table_allocate(ator, n_rows + 1);
    if (!new_table)
        return NULL;
    if (n_rows > 0) {
        /* copy over table */
        memcpy(&new_table[0], &TABLE(anchor)[0], pos * sizeof(HamtNode));
        /* note: this works since (n_rows - pos) == 0 for cases
         * where we're adding the new k/v pair at the end (i.e. memcpy(a, b, 0)
         * is a nop) */
        memcpy(&new_table[pos + 1], &TABLE(anchor)[pos],
               (n_rows - pos) * sizeof(HamtNode));
    }
    assert(!is_value(VALUE(anchor)) && "URGS");
    table_free(ator, TABLE(anchor), n_rows);
    TABLE(anchor) = new_table;
    INDEX(anchor) |= (1 << index);
    return anchor;
}

```

```c
HamtNode *table_shrink(struct HamtAllocator *ator, HamtNode *anchor,
                       size_t n_rows, uint32_t index, uint32_t pos)
{
    /* debug assertions */
    assert(anchor && "Anchor cannot be NULL");
    assert(!is_value(VALUE(anchor)) &&
           "Invariant: shrinking a table requires an internal node");

    HamtNode *new_table = NULL;
    uint32_t new_index = 0;
    if (n_rows > 0) {
        new_table = table_allocate(ator, n_rows - 1);
        if (!new_table)
            return NULL;
        new_index = INDEX(anchor) & ~(1 << index);
        memcpy(&new_table[0], &TABLE(anchor)[0], pos * sizeof(HamtNode));
        memcpy(&new_table[pos], &TABLE(anchor)[pos + 1],
               (n_rows - pos - 1) * sizeof(HamtNode));
    }
    table_free(ator, TABLE(anchor), n_rows);
    INDEX(anchor) = new_index;
    TABLE(anchor) = new_table;
    return anchor;
}

```

```c
HamtNode *table_gather(struct HamtAllocator *ator, HamtNode *anchor,
                       uint32_t pos)
{
    /* debug assertions */
    assert(anchor && "Anchor cannot be NULL");
    assert(!is_value(VALUE(anchor)) &&
           "Invariant: gathering a table requires an internal anchor");
    assert((pos == 0 || pos == 1) && "pos must be 0 or 1");

    int n_rows = get_popcount(INDEX(anchor));
    assert((n_rows == 2 || n_rows == 1) &&
           "Table must have size 1 or 2 to gather");
    HamtNode *table = TABLE(anchor);
    KEY(anchor) = table[pos].as.kv.key;
    VALUE(anchor) = table[pos].as.kv.value; /* already tagged */
    table_free(ator, table, n_rows);
    return anchor;
}

```

```c
HamtNode *table_dup(struct HamtAllocator *ator, HamtNode *anchor)
{
    int n_rows = get_popcount(INDEX(anchor));
    HamtNode *new_table = table_allocate(ator, n_rows);
    if (new_table) {
        memcpy(&new_table[0], &TABLE(anchor)[0], n_rows * sizeof(HamtNode));
    }
    return new_table;
}
```

## Putting it all together

### Search

### Insert

### Remove

### Iterators

## Persistent data structures and structural sharing

### Basic idea: path copying

### Insert

### Remove

# Appendix

## Unit testing

For testing, `hamt` uses a variant of [John Brewer's `minunit` testing
framework][brewer_xx_minunit]. Minunit is extremely minimalistic and its
header-only implementation easily fits on a single page:

```c
// test/minunit.h
#ifndef MINUNIT_H
#define MINUNIT_H

#define MU_ASSERT(test, message)                                               \
    do {                                                                       \
        if (!(test))                                                           \
            return message;                                                    \
    } while (0)
#define MU_RUN_TEST(test)                                                      \
    do {                                                                       \
        char *message = test();                                                \
        mu_tests_run++;                                                        \
        if (message)                                                           \
            return message;                                                    \
    } while (0)

#define MU_TEST_CASE(name) static char *name()
#define MU_TEST_SUITE(name) static char *name()

extern int mu_tests_run;

#endif /* !MINUNIT_H */
```

With `minunit`, every unit test is a `MU_TEST_CASE` We use `MU_ASSERT` to test
the test invariants.  Test cases are grouped into `MU_TEST_SUITE`s as
sequential calls to `MU_RUN_TEST`.  When an assertion fails, the `return`
statement in `MU_ASSERT` short-circuts test execution and returns a non-null
pointer to the respective `message` (generally a static string). This, in turn,
causes `MU_RUN_TEST` to issue a `return` call with the string pointer,
short-circuting the remaining test suite. The header also declares a global
variable `mu_tests_run` that keeps track of the total number of executed
tests.

The following listing illustrates the basic structure of unit test
implementations with `minunit`, check the [actual tests](test/test_hamt.c) for
a full listing.

```c
// test/test_hamt.c
#include "minunit.h"
#include "../src/hamt.c"

int mu_tests_run = 0;

MU_TEST_CASE(test_dummy)
{
    /* do something here */
    MU_ASSERT(0 == 0, "Oops X-{");
    return 0;
}

MU_TEST_SUITE(test_suite)
{
    /* Add tests here */
    MU_RUN_TEST(test_dummy);
    /*
     * ... many more ...
     */
    return 0;
}

int main()
{
    printf("---=[ Hash array mapped trie tests\n");
    char *result = test_suite();
    if (result != 0) {
        printf("%s\n", result);
    } else {
        printf("All tests passed.\n");
    }
    printf("Tests run: %d\n", tests_run);
    return result != 0;
}
```

Note that the test setup `include`s the `hamt.c` implementation file. This is a
common trick used in unit testing to gain easy access to testing `static`
functions that would otherwise be inaccessible since they are local to the
`hamt.c` compilation unit. This requires some care in
the Makefile setup in order to avoid symbol duplication.

[bagwell_00_ideal]: https://lampwww.epfl.ch/papers/idealhashtrees.pdf
[boehm_gc]: https://www.hboehm.info/gc/
[brewer_xx_minunit]: http://www.jera.com/techinfo/jtns/jtn002.html
[chaelim_hamt]: https://github.com/chaelim/HAMT
[coyler_15_champ]: https://blog.acolyer.org/2015/11/27/hamt/
[krukov_09_understanding]: http://blog.higher-order.net/2009/09/08/understanding-clojures-persistenthashmap-deftwice.html
[stutter]: https://github.com/mkirchner/stutter
[wiki_associative_array]: https://en.wikipedia.org/wiki/Associative_array
[wiki_hash_table]: https://en.wikipedia.org/wiki/Hash_table
[wiki_hash_tree]: https://en.wikipedia.org/wiki/Hash_tree_(persistent_data_structure)
[wiki_persistent]: https://en.wikipedia.org/wiki/Persistent_data_structure
[wiki_set_adt]: https://en.wikipedia.org/wiki/Set_(abstract_data_type)
[wiki_trie]: https://en.wikipedia.org/wiki/Trie


## Todo

### Basic implementation

- [ ] testing
  - [ ] add mem checks, possibly using the Boehm GC?
- [ ] docs

### Performance testing

- [x] set up perf test tooling
- [ ] implement perf tests suite

### Someday

* Add more iterator tests
* support key/value pairs and sets (?)
* typing solution (`#define *_TYPE` and `#include` approach?)

# Footnotes

<b id="fn_make">[1]</b> `make` first appeared in 1976, has (in numerous
incarnations) stood the tests of time and still is the most straightforward
approach for portable build specifications in small projects (and some would
argue in large ones, too).  [↩](#ac_make)

<b id="fn_void">[2]</b> At the expense of type safety and the ability to
perform any kind of static checking. There are alternate, more type-safe
solutions based on preprocessor-controlled code duplication for multiple types.
[↩](#ac_void)


