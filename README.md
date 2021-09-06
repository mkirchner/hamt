# hamt
A hash array-mapped trie implementation in C

## Todo

### Basic implementation

- [ ] add tests for depth>5
- [ ] `hamt_remove(key)`
- [ ] cleanup
  - hash state management
  - pass by value vs pass by ref
- [ ] iteration over contents (unsorted, stable)
- [ ] typing solution (`#define *_TYPE` and `#include` approach?)

### Immutability

- [ ] path copying for `set` and `remove`

### Performance

- [ ] set up perf test tooling
- [ ] decide on ref implementations
- [ ] implement perf tests suite


