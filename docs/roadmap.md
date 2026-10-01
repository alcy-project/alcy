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
  carried whole across calls (`docs/adr/0022-slices-as-views.md`). A fixed array decays
  at an argument, indexing bounds-checks the runtime length through an
  element place, and a view keeps the buffer's loan — `Vec::as_slice`,
  `as_mut_slice`, and `String::as_bytes` are the first borrowed APIs,
  pinned by `ok-slice`, `err-slice-realloc`, and `exe-slice`.
  Sub-slicing arrives with range data, the task below.
- [x] `Map` and `Set`, str-keyed with an internal hash.

  Landed as a `str`-keyed `Map<V>` with a companion `Set` over
  `Map<u8>` (`docs/adr/0024-str-keyed-collections-before-specs.md`). Keys are copied and owned, removal
  leaves a hole the next insertion reuses, and growth rehashes at
  three quarters occupancy. The key type stays `str` until `spec` can
  declare `Hash` and `Eq`; pinned by `exe-map`, `exe-set`, and the
  `map` cases in `tools/check_borrow_rules.py`.
- [x] `Range` data and sub-slicing.

  Range expressions build the interval types (`docs/adr/0025-ranges-as-data.md`), and an
  index accepts a run (`&a[1..<3]`, `s[1..]`), both endpoints
  bounds-checked. A range that names an end says how it is bound, so
  `..=` and `..<` are the only operators that may carry an endpoint
  and bare `..` is the unbounded spelling
  (`docs/adr/0032-explicit-range-end-spelling.md`), which also fixes
  the start side as always inclusive. `spec` and the `for` rule have
  since landed (`docs/adr/0028-spec-system.md`), and integer ranges
  iterate through their cursor
  (`docs/adr/0034-range-iteration-through-a-cursor.md`), so
  `for i in 0..<n` iterates. Pinned by
  `exe-range`, `exe-range-iter`, `exe-range-iter-no-start`,
  `exe-subslice`, `exe-subslice-panic`, the
  `ok-range`/`ok-range-iter`/`err-range-*` cases, and the `subslice`
  cases in `tools/check_borrow_rules.py`.
- [ ] Generic instances intern by structure, not by index.

  Two spellings of one instantiation must be one type, but tuple
  types are re-minted on each resolution while generic instances
  dedupe by exact argument index, so `Iterator<(i32, i32)>` fails its
  own signature check (`docs/adr/0031-for-desugar-in-the-parser.md`).
  The fix belongs with type-table interning rather than with any
  single rule, and unblocks tuple-item iterators everywhere.
- [ ] Refutable `for` patterns: filter or error.

  The pattern must match every item; whether a refutable pattern
  filters or is an error is undecided, so a program that builds with
  one is rejected by the lowerer for now. Decide it, specify it in
  `control.md`, and pin both sides with `for` cases.
- [ ] Strided range iteration.

  The cursor yields consecutive values; a stride is the remaining
  range API. Design it against the cursor first — the stride meets
  the inclusive-end rule the cursor already carries — then build it.
- [ ] `ArrayVec<T, N>`, a fixed-capacity inline container.

  The baremetal case proper: no heap, so no realloc hazard, and the
  growth logic it shares with `Vec`.
