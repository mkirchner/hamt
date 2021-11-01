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

1. The  `include/hamt.h` header file in which we define the public API.
   For now, we start with a very basic interface that we will re-visit
   [at a later point](#fixme).
2. A dummy implementation of the API functions defined above so we can
   successfully compile the project. The implementation will reside in
   `src/hamt.c`.
3. A simple test case, so we have something to run once the compilation
   is done.
4. A `Makefile` that ties it all together.

### Setup: the API

At the very least, the API needs to define a data structure (i.e. what will
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

### Setup: the implementation

We also add a preliminary implementation:

```c
struct HamtImpl {
    int dummy;
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

### Setup: Using `make` to build the project



## Design & foundational data structures

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
