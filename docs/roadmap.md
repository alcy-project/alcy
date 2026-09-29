# Roadmap

Near-term work on the language's foundations. Ordered, and one at a
time: the next task is the only one started, and the rest wait. The
peripheral work lives in `backlog.md`.

## Next

- [x] Growable `String`.

  Landed as a heap buffer mirroring `Vec<u8>`, with `format` measuring
  before it reserves so nothing truncates. A `str` view keeps its
  buffer's loan alive, so holding one across a `push` conflicts — the
  first exercise of the realloc-conflict rule on a type the standard
  library owns, pinned by `err-string-realloc`.
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
