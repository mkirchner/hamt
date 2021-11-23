# libhamt
A hash array-mapped trie (HAMT) implementation in C99. The implementation
follows Bagwell's 2000 paper [[1]][bagwell_00_ideal], with a focus on clarity
rather than raw speed.

The original motivation for this effort was the desire to implement an efficient
persistent data structure with structural sharing for maps and sets for [my own Lisp
implementation][stutter].

## Quickstart

```bash
$ git clone git@github.com:mkirchner/hamt.git
$ cd hamt
$ make
$ make test
```

In order to use `libhamt` in your own projects, copy `include/hamt.h` and
`src/hamt.c` in your own source tree and build from there.

## Table of Contents

The documentation starts with a introduction into hash array mapped
tries, giving an overview over the foundational building blocks (tries,
hashing) and how they come together in a HAMT.


## Introduction

A *hash array mapped trie (HAMT)* implements an [associative
array][wiki_associative_array].  HAMTs are a specific type of [hash
trees][wiki_hash_tree] that combine the characteristics of [hash
tables][wiki_hash_table] and array mapped [tries][wiki_trie].

The combination enables a advantageous trade-off between speed and memory
efficiency: HAMTs provide almost hash table-like time complexity guarantees
[[1]][bagwell_00_ideal] while making much more economic use of memory.
Additionally, combining the HAMT tree structure with path copying and garbage
collection, allows for a straightforward and efficient implementation of
[persistent][wiki_persistent] maps and sets.



[stutter]: https://github.com/mkirchner/stutter
[wiki_associative_array]: https://en.wikipedia.org/wiki/Associative_array
[wiki_hash_table]: https://en.wikipedia.org/wiki/Hash_table
[wiki_trie]: https://en.wikipedia.org/wiki/Trie
[wiki_hash_tree]: https://en.wikipedia.org/wiki/Hash_tree_(persistent_data_structure)
[wiki_persistent]: https://en.wikipedia.org/wiki/Persistent_data_structure

* Use cases
  * key/value store
* Key ideas
  * Rely on hash function for balancing (as opposed to RB/AVR etc trees)
  * 32-ary internal nodes, wide fan-out

# API

## HAMT lifecycle

```c
typedef struct HamtImpl *HAMT;
typedef int (*HamtCmpFn)(const void *lhs, const void *rhs);
typedef uint32_t (*HamtKeyHashFn)(const void *key, const size_t gen);


HAMT hamt_create(HamtKeyHashFn key_hash, HamtCmpFn key_cmp,
                 struct HamtAllocator *ator);
void hamt_delete(HAMT);
```
### Memory management

```c
struct HamtAllocator {
    void *(*malloc)(const size_t size);
    void *(*realloc)(void *chunk, const size_t size);
    void (*free)(void *chunk);
};

extern struct HamtAllocator hamt_allocator_default;
```

## Query

```c
size_t hamt_size(const HAMT trie);
const void *hamt_get(const HAMT trie, void *key);
```

```c
typedef struct HamtIteratorImpl *HamtIterator;

HamtIterator hamt_it_create(const HAMT trie);
void hamt_it_delete(HamtIterator it);
bool hamt_it_valid(HamtIterator it);
HamtIterator hamt_it_next(HamtIterator it);
const void *hamt_it_get_key(HamtIterator it);
const void *hamt_it_get_value(HamtIterator it);
```

## Modification

```c
const void *hamt_set(HAMT trie, void *key, void *value);
void *hamt_remove(HAMT trie, void *key);
```

## Using the HAMT as an efficient persistent data structure

```c
const HAMT hamt_pset(const HAMT trie, void *key, void *value);
const HAMT hamt_premove(const HAMT trie, void *key);
```

## Example: in-place modification w/ standard allocation

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

# Implementation

## Setup

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
files that are part of the public `hamt` interface; the `src` folder, for the
actual implementation and private header files; and the `test` folder, for all
test code, including headers and sources for testing utilities (e.g. data
loading and benchmarking functions).

The build process is governed by a single Makefile. While one could split the
Makefile by folder, the single-file solution is a better tradeoff for
simplicity.

### Unit testing with `minunit`

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

[brewer_xx_minunit]: http://www.jera.com/techinfo/jtns/jtn002.html

### Building the project

For the impatient:

```
$ make && make test
```

We use `make` as a build system<sup id="ac_make">[1](#fn_make)</sup>, with
three targets:
1. `make` or `make lib` builds the shared library `libhamt.dylib`
2. `make test` builds and executes the tests, and
3. `make perf` builds and executes the performance tests, and creates a simple
   box plot. This target requires a Python 3 installation w/ `matplotlib` and
   `pandas` packages.


## Design & foundational data structures

![](doc/img/hamt-overview.png)

* What is a trie?

* What is a hash trie?

distinction between trie with and without inner nodes

One of the potential drawbacks of hash tries is that they grow in depth
linearly with the length of the input. At their core, they are a
memory-efficient but not necessarily a search-efficient representation. Hash
tries partially remedy that situation: they use a hash function to pre-process
the value to be stored in the tree and use the bits of the hash to determine
the location of a particular value in the tree. The number of bits used at
every tree depth determines the fan out factor and the eventual depth of the
tree.

Hash array mapped tries take this idea into prac

enable shallower trees by
increasing the uniformity of the key distribution
A hash trie is a trie that uses the *hash* of a value to determine the
path to and position of the final node in a trie. 

* What is a hash array mapped trie?


* How do we represent the data structure in memory?

* Anchor view
* HamtNode definition

## Hashing

* what is a hash function?
* different classes: cryptographically secure, just efficient


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


### Hash generations and state management

One challenge with the use of pred
For a hash trie, the number of elements in the trie is limited by the total number
of hashes that fits into a 32-bit `uint32_t`, i.e. 2^32-1. Since the HAMT only
uses 30 bits (in 6 chunks of 5 bits), the number of unique keys in the trie is
limited to 2^30-1 = 1,073,741,823 keys. 
In a related fashion, since every layer of the
tree uses 5 bits of the hash, this limits the depth of the trie to 6 layers.
Neither the hard limit to the number of elements in the trie,
nor the inability to build a trie beyond depth 6 are desirable properties.

To address both issues, `hamt` recalculates the hash with a different seed every
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

## Putting it all together

### Search

### Insert

### Remove

### Iterators

## Persistent data structures and structural sharing

### Basic idea: path copying

### Insert

### Remove


[bagwell_00_ideal]: https://lampwww.epfl.ch/papers/idealhashtrees.pdf

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


