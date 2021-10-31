# hamt
A hash array-mapped trie implementation in C99. The implementation follows
the Bagwell 2001 paper [[1](bagwell_01_ideal)]

[bagwell_01_ideal]: https://lampwww.epfl.ch/papers/idealhashtrees.pdf




## Todo

### Basic implementation

- [x] add basic tests for depth>5
- [x] `hamt_remove(key)`
- [x] `hamt_size(handle)`
- [x] `hamt_delete(handle)`
- [x] Pull debug code from `hamt.c`
- [x] iteration over contents (unsorted, stable)
- [ ] Add more iterator tests
- [ ] support key/value pairs and sets (?)
- [ ] typing solution (`#define *_TYPE` and `#include` approach?)
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
