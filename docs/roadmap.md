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
- [x] Slices, `&[T]`.

  Landed as the generalization of `str`: an unsized `[T]` that only
  exists behind a reference, whose value is the `{ptr, len}` pair
  carried whole across calls (`docs/adr/0022`). A fixed array decays
  at an argument, indexing bounds-checks the runtime length through an
  element place, and a view keeps the buffer's loan — `Vec::as_slice`,
  `as_mut_slice`, and `String::as_bytes` are the first borrowed APIs,
  pinned by `ok-slice`, `err-slice-realloc`, and `exe-slice`.
  Sub-slicing waits for range endpoints.
- [ ] `Map` and `Set`, str-keyed with an internal hash.

  No traits. Self-hosting needs them.
- [ ] `ArrayVec<T, N>`, a fixed-capacity inline container.

  The baremetal case proper: no heap, so no realloc hazard, and the
  growth logic it shares with `Vec`.
