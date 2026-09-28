# Roadmap

Near-term work on the language's foundations. Ordered, and one at a
time: the next task is the only one started, and the rest wait. The
peripheral work lives in `backlog.md`.

## Next

- [ ] Loans exist wherever a reborrow happens.

  A reborrow at an argument or receiver position lowers to a `Load`,
  so the borrow checker never sees a `Borrow` and no loan is created;
  a function taking `&Self` and returning `&T` therefore hands back a
  pointer it never recorded as borrowed. `ElemOffset` and `TypeCast`,
  which the reborrow intrinsics lower to, are absent from the checker
  entirely, so `Vec::at`'s result carries no loan either. A `push` that
  reallocates goes undiagnosed and the program reads the freed block.
  `Borrow` at those positions, the two opcodes in the checker, and a
  regression test that reads `at`'s result across a reallocating
  `push`.

## Queue

- [ ] Growable `String`.

  `String` is a fixed 256-byte array, so a longer result panics
  instead of growing. `fmt`'s compile-time expansion writes the
  pieces into the `buf` field as an inline array, so a heap buffer
  means the expander allocates at run time; `docs/spec/fmt.md`
  records the two ways to agree the size, and the first — the format
  string is handed its scratch as an argument — is the honest one.
  Needs the loans above: without them this task's realloc conflicts
  would be written, and would not be detected.
- [ ] Slices, `&[T]`.

  The generalization of `str`, and the ground the borrowed APIs and
  the containers stand on. Confirm what of it already works before
  building: the indexing and slicing of a fixed array may carry most
  of the syntax, with the borrowed type and its lifetime the rest.
- [ ] `Map` and `Set`, str-keyed with an internal hash.

  No traits. Self-hosting needs them.
- [ ] `ArrayVec<T, N>`, a fixed-capacity inline container.

  The baremetal case proper: no heap, so no realloc hazard, and the
  growth logic it shares with `Vec`.
- [ ] The narrow scope for borrow against realloc.

  With loans in place, the remaining question is what a loan into a
  buffer has to say about a reallocation of it.
