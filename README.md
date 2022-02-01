# libhamt
A hash array-mapped trie (HAMT) implementation in C99. The implementation
follows Bagwell's 2000 paper[[1]][bagwell_00_ideal], with a focus on clarity
rather than raw speed.

A HAMT is a data structure that can be used to efficiently implement
[*persistent*][wiki_persistent_data_structure] associative arrays (aka maps)
and sets, see the [Introduction](#introduction).

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
/* The libhamt core data structure is a handle to a hash array-mapped trie */
typedef struct HamtImpl *HAMT;

/* Function signature definitions for key comparison and hashing */
typedef int (*HamtCmpFn)(const void *lhs, const void *rhs);
typedef uint32_t (*HamtKeyHashFn)(const void *key, const size_t gen);

/* API functions for lifecycle management */
HAMT hamt_create(HamtKeyHashFn key_hash, HamtCmpFn key_cmp, struct HamtAllocator *ator);
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

**Hash tables.** A common and practical answer to efficient value retrieval
from a collection given a key is to "use a *hash table*".  This is good
advice. *Hash tables* provide insert, modification, and retrieval in amortized
constant average time, using space linear in the number of elements they
store.  They have been the subject of intensive research and
optimization and are part of [every][sedgewick_11_algorithms]
[introductory][cormen_09_introduction] CS textbook.  Chances are that the
standard library of the languange at hand contains a readily available, tried
and tested implementation.

For instance, `std::unordered_set` and `std::unordered_map` (and their
`*_multiset` cousins) are hash table implementations for C++ <sup
id="ac_hash_table_cpp">[1](#fn_hash_table_cpp)</sup>; for C, multiple
[libc][wiki_libc] implementations (e.g. [glibc][wiki_glibc], [musl][musl],
[BSD libc][wiki_bsd_libc]) provide POSIX-compliant `hsearch` facilities,
GNOME's [GLib][wiki_glib]
and others provide [hash table][glib_hashtable] implementations<sup
id="ac_hash_table_c">[2](#fn_hash_table_c)</sup>. Python has the `dict` type
for associative arrays which [is implemented as a hash
table][python_dict_pre36]<sup
id="ac_hash_table_python">[3](#fn_hash_table_python)</sup>.  Java has
`Hashtable`, `HashMap`, and `HashSet` <sup
id="ac_hash_table_java">[4](#fn_hash_table_java)</sup> and JavaScript has
[`Map`][js_map].

One property of the classical hash table implementations is that they do not
provide support for *persistence* (in the sense of [persistent data
structures][wiki_persistent], not persistent storage). They are a
[place-oriented][hickey_value_of_values] solution to associative storage and
make destructive modifications to the data structure when the data changes
(note that this is independent of any particular conflict resolution and
capacity maintenance strategies).

Persistent associative containers require a different approach.

**Persistent data structures.** *(Full) persistence* is the property of a data
structure to always preserve (all) previous versions if itself under
modification. The property is related to
[immutability][wiki_immutable_object]: from the perspective of the client,
every update yields a new copy, making instances practically immutable. This
is a huge conceptual change: if data structures are immutable, functions using
these data structures are pure (i.e. side effect-free). That in turn enables
[value semantics][wiki_value_semantics], [referential
transparency][wiki_referential_transparency], and, consequently, substantial
reduction in programming complexity when dealing with paralellism and
synchronization (see e.g. Rich Hickey's presentations on [*The Value of
Values*][hickey_value_of_values] and [*Are We There
Yet?*][hickey_are_we_there_yet]).

The catch is that classical hash tables set a high bar in terms of time and
space performance characteristics, and persistent data structures need to
approximate that bar.

**Efficient persistence.** Persistent associative data structures need to
minimize the memory overhead introduced by value
semantics (i.e. returning copies as opposed to modified originals) and, at
the same time, provide practically average constant time insert, retrieve and
delete capabilities to minimize the performance gap to classical hash tables.

It turns out that the data structure of choice to tackle these challenges is a
*tree*. Trees support efficient [*structural
sharing*][wiki_structural_sharing] strategies for efficient memory management
and, if they are *balanced* and have *large branching factors*, provide
O(log<sub>k</sub> N) average performance guarantees.

*Persistent hash array-mapped tries* are, in essence, a sophisticated,
practical implementation of such a data structure.


### Persistent Hash Array-Mapped Tries

One way to understand hash array-mapped tries is to look at them as an
evolution of *k*-ary trees (Fig. 1) that follows from a series of real-world
tradeoffs.

<p align="center">
<img src="doc/img/hamt-trees.png" width="600"></img>
</p>
<p class="image-caption"><b>Figure 1:</b> *k*-ary tree, hash tree, and
hash array-mapped trie.</p>

In classic *k*-ary trees Ⓐ,  Internal and leaf nodes have
different types: internal nodes point to *n* internal or leaf nodes and leaf
nodes hold or point to data (i.e. the keys/value pairs). In their basic form,
*n*-ary trees (just like binary trees) are not balanced and their performance
characteristics can easily degrade from *O(log<sub>k</sub> n)* to *O(n)*
for degenerate input sequences.

One approach to balanced trees are explicit implementations of
tree rebalancing (as in e.g. [Red-black
trees][wiki_red_black_trees], [AVL trees][wiki_avl_trees], or
[B-trees][wiki_b_trees]).

Another option is to use a [*hash tree*][wiki_hash_tree] Ⓑ: like the name
implies, it uses the *hash* of the key, interpreted as a sequence of *b*-bit
groups, to detetermine the location of the leaf node that stores the key/value
pair. The group size *b* determines the branching factor 2<sup><i>b</i></sup>,
i.e. for *b*=5, every node can have 2<sup>5</sup>=32 child nodes.
Instead of implementing explicit tree rebalancing, hash trees rely on the
distributional properties of a (good) hash function to place nodes uniformly.
While this saves some effort for rebalancing, note that hash trees *do*
require a strategy to deal with *hash exhaustion*, a topic covered below.

The challenge with vanilla hash trees is that they reserve space for *k*
children in every internal node. If the tree is sparsely populated this will
cause significant memory overhead and impact performance due to cache misses.

For that reason, HAMTs implement *array mapping* Ⓒ: instead of reserving space
for *n* pointers to children in each internal node, the parent node stores a
bitmap that indicates which children are present and the actual node only
allocates the memory required to refer to its children. This is an important
optimization that makes trees with a high branching factor more memory
efficient and cache-friendly.

In order to implement a *persistent* map or set, every modification operation
must return a modified copy and maintain the source data structure. And
returning actual copies is prohibitively expensive in time and memory.

This, finally, is where HAMTs really shine and the true reason why we build
them in the first place.

HAMTs are trees and trees are very compatible with
[structural sharing][wiki_persistent_data_structure] strategies. Common
techniques are copy-on-write, fat nodes, [path
copying][wiki_persistent_structural_sharing], and there are [complex
combinations of the previous three][driscoll_86_making]. Path copying is
simple, efficient and general and therefore the technique of choice for
`libhamt`: Instead of returning an actual copy of the tree during an insert,
update or delete operations, we follow the search path to the item in
question, maintaining a path copy with all the nodes along the way, make our
modification along this path and return it to the caller.

Note that enabling persistence *requires* the use of a garbage collection
strategy. Under stanard `malloc()` memory management, there is no way for
the HAMT nodes to know how many descendants of a HAMT refer to them.

### Implementation strategy

In the following we will address these concepts in turn: we first define the
foundational data structure used to build a tree and introduce the concept of
an *anchor*. We then dive into hash functions and the *hash state management*
required to make hashing work for trees of arbitrary depths and in the
presence of hash collisions. Lastly, we turn to *table management*,
introducing a set of functions used to create, modify, query and dispose of
mapped arrays.  With these pieces in place, we are ready to implement the
insert/update, query, and delete functions for non-persistent HAMTs. And
lastly, we will then introduce the concept of path copying and close with the
implementation of persistent insert/update and delete functions for HAMTs.


### Foundational data structures
<!--
<p align="center">
<img src="doc/img/hamt-overview.png" width="600"></img>
</p>
<p class="image-caption"><b>Figure 1:</b> HAMT data structure.
<code>libhamt</code> implements
HAMTs using linked, heap-allocated tables. Table rows hold
either an index vector and pointer to a subtable or pointers to key and
value (one pair of key/value pointers illustrated in blue, and implicit to all
empty table fields).</p>
-->

`libhamt` uses different types to implement internal and leaf nodes.

Leaf nodes contain two fields, called `value` and `key` (the rationale for the
reverse ordering of the two fields will become evident shortly).
```c
struct {
    void *value;
    void *key;
} kv;
```
Both fields are
defined as `void*` pointers to support referring to arbitrary data types via
type casting
<sup id="ac_cpp_virtual_method_table">[5](#fn_cpp_virtual_method_table)</sup>.

`libhamt`'s internal nodes are where the magic happens, based on Bagwell's *[Ideal Hash
Trees][bagwell_00_ideal]* paper and according to the design principles
outlined above.

With a branching factor *k*, internal nodes have at most *k* successors but
can be sparsely populated. To allow for a memory-efficient representation,
internal nodes have a pointer `ptr` that points to a fixed-size, right-sized
*array* of pointers to the child nodes and a *k*-bit `index` bitmap field that
keeps track of the size and occupancy of that array.
Because `index` is a bitmap field, the number of one-bits in `index` yields
the size of the array that `ptr` points to.

`libhamt` uses *k*=32.

This suggests an initial (incomplete) definition along the following lines:
```c
struct {
    struct T *ptr;  /* incomplete */
    uint32_t index;
} table;
```

The specification of `T` must provide the ability for that datatype to point to
internal and external nodes alike, using only a single pointer type.
A solution is to wrap the two types into a `union` (and then to wrap
the `union` into a `typedef` for convenience):

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

With this structure, given a pointer `HamtNode *p` to a `HamtNode`
instance, `p->as.kv` addresses the leaf node, and `p->as.table` addresses the
internal node and `p->as.kv.value`, `p->as.kv.key`, `p->as.table.ptr`, and
`p->as.table.index` provide access to the respective fields.

To maintain sanity, we define the following convenience macros:

```c
#define TABLE(node) node->as.table.ptr
#define INDEX(node) node->as.table.index
#define VALUE(node) node->as.kv.value
#define KEY(node)   node->as.kv.key
```

<p align="center">
<img src="doc/img/hamtnode-table.png" width="300"></img>
</p>
<p class="image-caption"><b>Figure 2:</b> 
Memory structure of an internal node. If <code>node</code> is a pointer
to an internal node, <code>TABLE(node)</code> (or, equivalently, <code>
node->as.table.ptr</code>) points to the first field of the successor table.
</p>

### Pointer tagging

The definition of `HamtNode` enables the construction of trees with a mix of
internal and leaf nodes. What the definition does not provide, is a way to
determine if a concrete `HamtNode*` pointer points to an internal or a leaf
node. One solution would be to to specify an `enum` that indicates the type
(i.e. `NODE_LEAF`, etc.) and to add a `type` field to `struct HamtNode`.  While
valid, this would also increase the size of the struct by 50% just to maintain
a single bit of information. Thankfully, there is a more memory-efficient
solution: pointer tagging.

Since pointers need to be word-aligned, that leaves the lower 3 bits of all
pointers on 64-bit architectures always set to zero. It is possible to make
use of these bits under two conditions: (1) we know we are looking at a
pointer (the bottom three bits for the integer 1 are zero, too); and (2) we
carefully mask the bits in question whenever we actually use the pointer
(since it would point to the wrong location otherwise). The first is not a
problem since we own the code; the second requires diligence and some helper
functions:

```c
#define HAMT_TAG_MASK 0x3
#define HAMT_TAG_VALUE 0x1
#define tagged(__p) (HamtNode *)((uintptr_t)__p | HAMT_TAG_VALUE)
#define untagged(__p) (HamtNode *)((uintptr_t)__p & ~HAMT_TAG_MASK)
#define is_value(__p) (((uintptr_t)__p & HAMT_TAG_MASK) == HAMT_TAG_VALUE)
```

In order to mark a leaf node as such, we set `key` as usual and tag the value
pointer before assigning it to `value`:

```c
    p->as.kv.key = key_ptr;
    p->as.kv.value = tagged(value_ptr);
```

Given a pointer to a leaf (e.g. a search result), we untag `value` before
returning it: 

```c
    ...
    if (status == SEARCH_SUCCESS) {
        return untagged(p->as.kv.value);
    }
    ...
```

And, in order to determine what we are looking at, we use `is_value`:

```c
    if (is_value(p->as.kv.value)) {
        /* this is a leaf */
        ...
    } else {
        /* this is an internal node */
        ...
    }
```

Pointer tagging is the explanation for the ordering of the `value` and `key`
fields in the `struct kv` struct. The `union` in `HamtNode` states that the
memory location of the `struct kv` and `struct table` structs overlap. Since
the `table.index` field is *not* a pointer (and the bottom-three-bits-are-zero
guarantee does not apply), its storage location cannot be used for pointer
tagging, leaving the `table.ptr` to the task.  Putting `kv.value` first,
aligns the value field with `table.ptr`. The reverse order would work, but the
`kv.key` pointer is dereferenced much more often in the code and so it is more
convenient to use `kv.value`.


### The Anchor

The `libhamt` codebase makes liberal use of the concept of an *anchor*.  An
*anchor* is a `HamtNode*` pointer to an internal node (i.e.
`is_value(VALUE(anchor))` evaluates to false). An `anchor` provides access to
all information relevant to manage the table of child nodes: `INDEX(anchor)`
returns the bitmap that encodes the array mapping, applying a popcount to the
bitmap gives the size of the table and indexing is implemented using partial
popcounts. Table elements can be accessed through
`TABLE(anchor)[i]`, where `i` must be in the valid range.

### Array mapping

```c
static int get_popcount(uint32_t n) { return __builtin_popcount(n); }

static int get_pos(uint32_t sparse_index, uint32_t bitmap)
{
    return get_popcount(bitmap & ((1 << sparse_index) - 1));
}

static inline bool has_index(const HamtNode *anchor, size_t index)
{
    assert(anchor && "anchor must not be NULL");
    assert(index < 32 && "index must not be larger than 31");
    return INDEX(anchor) & (1 << index);
}
```
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
    int n_rows = get_popcount(INDEX(anchor));
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


# Footnotes

<b id="fn_hash_table_cpp">[1]</b>
The `std::unordered_*` methods implement open hashing (aka separate chaining),
with the hash table being an array of buckets, each pointing to the head of a
linked list. This is a deliberate and reasonable compromise for general use;
gaining an order of magnitude of speed improvements for specialized use cases
(e.g. append-only, guaranteed high-quality hash functions) is possible. See
[this stackoverflow post][cpp_unordered_map_impl] for a summary of the [standard
proposal][austern_03_proposal].
[↩](#ac_hash_table_cpp)

<b id="fn_hash_table_c">[2]</b>
`musl` provides a `hsearch` implementation that uses closed hashing with
quadratic probing for conflict resolution. The
[documentation][musl_libc_hsearch] states that they use powers of two for
table sizing which seems wrong due to the impact on the modulo (table sizes
should ideally be prime). The GLib `GHashTable` has surprisingly little
documentation in its implementation details but [appears to be
using][glib_hashtable] a separate chaining approach similar to the C++
solution.
[↩](#ac_hash_table_c)
  
<b id="fn_hash_table_python">[3]</b> Python's `dict` implementation uses
closed hashing (aka open addressing) with pseudo-random probing to mitigate
the poor hashing properties of standard python `hash()` function for some data
types (from [here][python_dict_pre36]). Python keeps the load factor below
0.66; this avoids gradual performance degradation associated w/ high load
factors in closed hashing but comes at increased memory footprint. The
[codebase][python_dictobj] was refactored to split the actual data from the
hash table in 3.6, resulting in better memory efficiency and GC friendliness
(see [here][python_dict_impl36] and [here][python_dict_impl36_2]).
[↩](#ac_hash_table_python)

<b id="fn_hash_table_java">[4]</b> Java provides `Hashtable<K,V>` and
`HashMap<K,V>`, both of which implement `Map` and `Collection` interfaces; in
addition, `Hashtable` is synchronized. The `HashSet` type internally uses a
`HashMap`. `Hashtable` and `HashMap` implement open hashing
(separate chaining) with a default load factor of 0.75; The OpenJDK
implementation of `HashMap` converts
between linked list and tree representations in the hash buckets, depending on
bucket size, see [the source][openjdk_java_util_hashmap].
[↩](#ac_hash_table_java)

<b id="fn_cpp_virtual_method_table">[5]</b>
There are alternative approaches to enable (somewhat) typesafe templating in
C, mainly by implementing what basically amounts to virtual method tables
using the C preprocessor. See e.g. [here][cpp_vmts] for a useful stackoverflow
summary or [here][c_templating] for a more in-depth treatise.
[↩](#ac_cpp_virtual_method_table)

[cpp_vmts]: https://stackoverflow.com/questions/10950828/simulation-of-templates-in-c-for-a-queue-data-type/11035347
[c_templating]: http://blog.pkh.me/p/20-templating-in-c.html


[austern_03_proposal]: http://www.open-std.org/jtc1/sc22/wg21/docs/papers/2003/n1456.html
[bagwell_00_ideal]: https://lampwww.epfl.ch/papers/idealhashtrees.pdf
[boehm_gc]: https://www.hboehm.info/gc/
[brewer_xx_minunit]: http://www.jera.com/techinfo/jtns/jtn002.html
[chaelim_hamt]: https://github.com/chaelim/HAMT
[cormen_09_introduction]: https://www.amazon.com/Introduction-Algorithms-3rd-MIT-Press/dp/0262033844/ref=zg_bs_491298_1/147-2375898-2942653?pd_rd_i=0262033844&psc=1
[coyler_15_champ]: https://blog.acolyer.org/2015/11/27/hamt/
[cpp_unordered_map_impl]: https://stackoverflow.com/a/31113618
[driscoll_86_making]: https://www.cs.cmu.edu/~sleator/papers/another-persistence.pdf
[glib_hashtable]: https://gitlab.gnome.org/GNOME/glib/-/blob/main/glib/ghash.c
[hickey_are_we_there_yet]: https://github.com/matthiasn/talk-transcripts/blob/master/Hickey_Rich/AreWeThereYet.md
[hickey_value_of_values]: https://github.com/matthiasn/talk-transcripts/blob/master/Hickey_Rich/ValueOfValues.md
[js_map]: https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/Map
[krukov_09_understanding]: http://blog.higher-order.net/2009/09/08/understanding-clojures-persistenthashmap-deftwice.html
[musl]: https://www.musl-libc.org
[musl_libc_hsearch]: https://git.musl-libc.org/cgit/musl/tree/src/search/hsearch.c
[openjdk_java_util_hashmap]: https://github.com/openjdk/jdk17/blob/74007890bb9a3fa3a65683a3f480e399f2b1a0b6/src/java.base/share/classes/java/util/HashMap.java
[python_dict_impl36]: https://morepypy.blogspot.com/2015/01/faster-more-memory-efficient-and-more.html
[python_dict_impl36_2]: https://mail.python.org/pipermail/python-dev/2012-December/123028.html
[python_dict_pre36]: https://stackoverflow.com/a/9022835
[python_dictobj]: https://github.com/python/cpython/blob/main/Objects/dictobject.c
[sedgewick_11_algorithms]: https://www.amazon.com/Algorithms-4th-Robert-Sedgewick/dp/032157351X
[stutter]: https://github.com/mkirchner/stutter
[wiki_associative_array]: https://en.wikipedia.org/wiki/Associative_array
[wiki_bsd_libc]:https://en.wikipedia.org/wiki/C_standard_library#BSD_libc
[wiki_glib]: https://en.wikipedia.org/wiki/GLib
[wiki_glibc]: https://en.wikipedia.org/wiki/Glibc
[wiki_hash_table]: https://en.wikipedia.org/wiki/Hash_table
[wiki_hash_tree]: https://en.wikipedia.org/wiki/Hash_tree_(persistent_data_structure)
[wiki_immutable_object]: https://en.wikipedia.org/wiki/Immutable_object
[wiki_libc]: https://en.wikipedia.org/wiki/C_standard_library
[wiki_persistent]: https://en.wikipedia.org/wiki/Persistent_data_structure
[wiki_persistent_data_structure]: https://en.wikipedia.org/wiki/Persistent_data_structure 
[wiki_persistent_structural_sharing]: https://en.wikipedia.org/wiki/Persistent_data_structure#Techniques_for_preserving_previous_versions
[wiki_referential_transparency]: https://en.wikipedia.org/wiki/Referential_transparency
[wiki_set_adt]: https://en.wikipedia.org/wiki/Set_(abstract_data_type)
[wiki_structural_sharing]: https://en.wikipedia.org/wiki/Persistent_data_structure#Trees
[wiki_trie]: https://en.wikipedia.org/wiki/Trie
[wiki_value_semantics]: https://en.wikipedia.org/wiki/Value_semantics
[wiki_avl_trees]: https://en.wikipedia.org/wiki/AVL_tree
[wiki_red_black_trees]: https://en.wikipedia.org/wiki/Red–black_tree
[wiki_b_trees]: https://en.wikipedia.org/wiki/B-tree

