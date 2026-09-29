# Roadmap

Near-term work on the language's foundations. Ordered, and one at a
time: the next task is the only one started, and the rest wait. The
peripheral work lives in `backlog.md`.

## Next

- [ ] The relational part of the region model.

  A reborrow carries its parent's loans, so the outlives constraint
  between them holds by construction rather than by being solved, and a
  struct holding a reference composes by whichever rule the flow happens
  to take rather than by the intersection `docs/spec/ownership.md`
  states. A summary that named the place a returned reference came from
  would also let an accessor's index reach the call site, where today a
  literal index is invisible because the accessor reads it as a
  parameter. No case in the rule matrix observes any of this, so it is a
  change with no failing case to point at — which is why it needs its
  own scope rather than riding along with a fix.

## Queue

- [ ] The relational part of the region model.

  A reborrow carries its parent's loans, so the outlives constraint
  between them holds by construction rather than by being solved, and a
  struct holding a reference composes by whichever rule the flow happens
  to take rather than by the intersection `docs/spec/ownership.md`
  states. Neither is observable yet through the rule matrix, so this is
  a change with no failing case to point at, which is why it waits
  behind work that does have one.
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
