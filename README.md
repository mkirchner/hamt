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

`hamt` uses the `minunit` testing framework for unit testing. Minunit is a very
minimalistic framework and its implementation fits on a single page of a single
header file (in our case: `minunit.h`):

```c
#ifndef MINUNIT_H
#define MINUNIT_H

/*
 * Based on: http://www.jera.com/techinfo/jtns/jtn002.html
 */

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

With `minunit`, every unit test is a `MU_TEST_CASE` which are grouped into
`MU_TEST_SUITE`s. The folliwing listing shows the basic structure of unit test
implementations with `minunit`, check the [actual tests]() for a full listing.
Also note that the unit tests include the `hamt.c` implementation file (as
opposed to just the header). This is a common pattern to enable testing access
to functions that would otherwise be local to the compilation unit (i.e.
functions declared as  `static`).

```c
#include "minunit.h"
#include "../src/hamt.c"

int mu_tests_run = 0;

MU_TEST_CASE(dummy)
{
    /* do something here */
    MU_ASSERT(0 == 0, "Zero is nonzero, that's bad");
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

### Setup: Using `make` to build the project

`hamt` uses `make` as a build system for its simplicity in small projects and
its portability. The Makefile is straightforward, albeit slightly verbatim.


## Design & foundational data structures

![](doc/img/hamt-overview.png)

* The trie


* Anchor view
* HamtNode definition

## Hashing

## Table management

## Putting it all together

### Search

### Insert

### Remove

### Iterators

## Persistent data structures and structural sharing

### Insert

### Remove


[bagwell_01_ideal]: https://lampwww.epfl.ch/papers/idealhashtrees.pdf

## Todo

### Basic implementation

- [x] add basic tests for depth>5
- [x] `hamt_remove(key)`
- [x] `hamt_size(handle)`
- [x] `hamt_delete(handle)`
- [x] Pull debug code from `hamt.c`
- [x] iteration over contents (unsorted, stable)
- [ ] clean up
  - [ ] anchor concept vs. table gather
  - [ ] hash state management (pass by value vs pass by ref)
  - [ ] nested conditional in inner remove logic is ugly
  - [ ] TABLE(root) vs root in non-persistent vs persistent case checks
- [ ] testing
  - [ ] add mem checks
  - [x] set up github actions
- [ ] docs

### Optimization

- [ ] add custom allocator
- [ ] remove recursion?

### Performance testing

- [x] set up perf test tooling
- [ ] decide on ref implementations
- [ ] implement perf tests suite

### Immutability

- [x] path copying for `set`
- [x] path copying for `remove`

### Someday

* Add more iterator tests
* support key/value pairs and sets (?)
* typing solution (`#define *_TYPE` and `#include` approach?)
