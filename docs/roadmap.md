# Roadmap

Near-term work on the language's foundations. Ordered, and one at a
time: the next task is the only one started, and the rest wait. The
peripheral work lives in `backlog.md`.

## Next

- [ ] A loan's extent is a region, not a span.

  A loan's extent is computed per function, so one live on a single
  branch is taken as live on all of them and an early return on a branch
  where the loan is already dead reports a conflict that is not one.
  This is the region solver's input, and the widest remaining piece of
  ADR-0012. The loans themselves now exist wherever a reborrow happens,
  so this is the only thing standing between the checker and programs
  that read through a loan across a branch.

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
