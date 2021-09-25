# hamt
A hash array-mapped trie implementation in C. This is work in progress.

## Todo

### Basic implementation

- [x] add basic tests for depth>5
- [x] `hamt_remove(key)`
- [ ] `hamt_size(handle)`
- [ ] `hamt_delete(handle)`
- [ ] iteration over contents (unsorted, stable)
- [ ] typing solution (`#define *_TYPE` and `#include` approach?)
- [ ] clean up
  - [ ] anchor concept vs. table gather
  - [ ] hash state management (pass by value vs pass by ref)
- [ ] testing
  - [ ] add mem checks
  - [ ] set up github actions
- [ ] docs

### Performance testing

- [ ] set up perf test tooling
- [ ] decide on ref implementations
- [ ] implement perf tests suite

### Immutability

- [ ] path copying for `set` and `remove`


