# hamt
A hash array-mapped trie (HAMT) implementation in C99. The implementation
follows the Bagwell 2001 paper [[1]][bagwell_01_ideal].

* educational implementation, focus on clarity, portability; decision to
  implement with recursion
* initial incentive: building my own Lisp, wanted a high-performance data
  structure that supports structural sharing for persistent data structures

## Structure

The documentation starts with a introduction into hash array mapped
tries, giving an overview over the foundational building blocks (tries,
hashing) and how they come together in a HAMT.

## Table of Contents

## Waht's a HAMT?

* Key ideas
  * Rely on hash function for balancing (as opposed to RB/AVR etc trees)
  * 32-ary internal nodes, wide fan-out

# Implememntation

## Project setup

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

In order to get started, we need four pieces:

1. The  `include/hamt.h` header file in which we define the public interface.
   For now, we start with a very basic interface that we will re-visit
   [at a later point](#fixme).
2. A dummy implementation of the functions defined in the interface so we can
   successfully compile the project. The implementation will reside in
   `src/hamt.c`.
3. A simple test case, so we have something to run once the compilation
   is done.
4. A `Makefile` that ties it all together.

### Setup: the `hamt.h` header file

At the very least, the interface needs to define a data structure (i.e. what will
eventually be our HAMT implementation) and a set of functions that operate on
it:

```c
#ifndef HAMT_H
#define HAMT_H

#include <stddef.h>
#include <stdint.h>

typedef struct HamtImpl *HAMT;

HAMT hamt_create();
void hamt_delete(HAMT);
```

### Setup: the `hamt.c` implementation file

We also add a preliminary implementation so we can make sure the build system
works:

```c
struct HamtImpl {
    void* dummy;
};


HAMT hamt_create()
{
    return (struct HamtImpl*) malloc(sizeof(struct HamtImpl));
}

void hamt_delete(HAMT h)
{
    free(h):
}
```

### Setup: the `minunit` framework and out first test

For unit testing, `hamt` uses a variant of [John Brewer's `minunit` testing
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

The folliwing listing shows the basic structure of unit test
implementations with `minunit`, check the [actual tests](../test) for a full listing.

Note that the test setup
`include`s the `hamt.c` implementation file. This is a common trick used in unit
testing to gain easy access to testing `static` functions that would otherwise be
inaccessible since they are not defined in the `hamt.h` header file.

```c
#include "minunit.h"
#include "../src/hamt.c"

int mu_tests_run = 0;

MU_TEST_CASE(dummy)
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

Including the `hamt.c` implementation file requires a bit of care in
the Makefile setup in order to avoid symbol duplication.

[brewer_xx_minunit]: http://www.jera.com/techinfo/jtns/jtn002.html

### Setup: Using `make` to build the project

`hamt` uses `make` as a build system for its simplicity in small projects and
its portability. The Makefile is straightforward, albeit slightly verbatim.


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


[bagwell_01_ideal]: https://lampwww.epfl.ch/papers/idealhashtrees.pdf

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
