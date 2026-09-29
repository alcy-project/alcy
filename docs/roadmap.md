# Roadmap

Near-term work on the language's foundations. Ordered, and one at a
time: the next task is the only one started, and the rest wait. The
peripheral work lives in `backlog.md`.

## Next

- [ ] A reborrow shortens the loan it derives from.

  A reborrow stands behind the loan it came from, and while it is live
  the two are the same loan as far as a write is concerned: the checker
  has to keep them apart, or a reborrow appears to conflict with the
  borrow it was taken from. This is rule 1 of ADR-0012, and it is what
  turns the liveness the checker now computes into a solved region: a
  derived loan's extent ends where the original's does rather than
  where its own last use falls.

## Queue

- [ ] Growable `String`.

  `String` is a fixed 256-byte array, so a longer result panics
  instead of growing. `fmt`'s compile-time expansion writes the
  pieces into the `buf` field as an inline array, so a heap buffer
  means the expander allocates at run time; `docs/spec/fmt.md`
  records the two ways to agree the size, and the first — the format
  string is handed its scratch as an argument — is the honest one.
  A heap buffer puts `String` in the position `Vec<T>` was in, so this
  is the first exercise of the realloc-conflict rule on a type the
  standard library owns.
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
