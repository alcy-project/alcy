# Types (MVP)

## Numeric tower

- Integers `i8`-`i64`, `u8`-`u64`, `isize`/`usize`; floats `f32`/`f64`;
  `bool`. `f16`, 128-bit integers, posits, and decimals are deferred.
- Literals: decimal, `0b`/`0o`/`0x`, type suffixes (`42i32`, `1.5f64`),
  `_` separators.
- Overflow: `+`, `-`, and `*` wrap in all modes. Division by zero
  and overshifts are unchecked with backend-defined behavior; traps
  for both are later work. No independent overflow-check flag exists;
  const evaluation follows the same semantics.

## Tuples

- Tuple types `(T, U)` are structural and concrete (no polymorphism
  in MVP): construction, `.0` access, and destructuring. The unit
  type `()` is the empty tuple.

## Fixed arrays

- Array types `[T; N]` are fixed-size and homogeneous; `N` is a
  decimal length. Literals are lists (`[a, b]`) or repeats (`[e; N]`,
  evaluated once); empty literals are rejected.
- Indexing reads and writes through places with panic-on-out-of-bounds
  semantics. Arrays are `Copy` if and only if their element is.

## Slices

- A slice `[T]` is a run of `T` of runtime length. It is unsized, so
  it appears only behind a reference: `&[T]` or `&mut [T]`. A bare
  `[T]` in a type position is rejected.
- A slice reference is a pointer and a length travelling together, so
  a view crosses a call boundary whole. A fixed array `[T; N]` decays
  to `&[T]` (or `&mut [T]`) at an argument position, keeping the
  borrow's kind and moving the length into the view.
- Indexing a slice bounds-checks against the runtime length, like a
  fixed array against its count. `&mut [T]` indexes writable, `&[T]`
  read-only.
- A slice is a view, not an owner: `&[T]` copies, `&mut [T]` is
  move-only, and the view keeps the loan of the buffer it reads (see
  `ownership.md`). `str` stays its own type rather than spelling as
  `&[u8]`, with the same representation and the same loan rule.
- Sub-slicing names a run with a range: `&a[1..<3]` is the run of a
  fixed array as a `&[T]` (or `&mut [T]`), `s[1..]` re-slices a view
  to its own kind, and a `str` narrows to a `str`. Both endpoints are
  bounds-checked and the run must be ordered, else the program panics
  with "slice out of bounds".

## Text (staged)

- The compiler-known text type is `str`: byte sequences backing
  string literals. No validation is performed in MVP; `Char` and the
  core text library live post-MVP. Baremetal targets without core
  use `u8`/`u32` directly.

## Ranges (staged)

- A range is interval data, not an iterator: a single
  `Range<T> { start: Bound<T>, end: Bound<T> }` with
  `Bound = Included(T) | Excluded(T) | Unbounded`, both declared by
  `core` with reserved names so a range expression always constructs
  the one declaration. A range that names an end says how it is
  bound: `..=` includes it and `..<` excludes it, and no other
  operator may be followed by an endpoint, so the choice is never
  implicit. A present start endpoint is always `Included` — no
  spelling excludes one — and an absent endpoint is `Unbounded`,
  which bare `..` alone spells.
- Both endpoints share one element type: an expected `Range<E>` pins
  it, otherwise the present endpoints agree, with a bare integer
  literal adapting to the other side as in a binary operation. With
  neither endpoint present (`..`) the unsuffixed default applies.
  Only integer ranges are indexable; float ranges are legal data with
  no index meaning.
- A range expression builds the value; an index with a range reads
  the run it names, `[start, end)` for `..<` and `[start, end]` for
  `..=`. A bare run of a fixed array is unsized, so only a borrow
  names it (`&a[1..<3]`); borrowing a view
  is rejected, since no place stands behind one. A run is not a place
  and cannot be assigned to.
- Iteration is explicit and separate: core declares `Iterator` as
  the capability, and only integer ranges expose one, through a
  `RangeIter` cursor built by `into_iter`; float ranges have no
  iteration method. The cursor ascends from the start and stops at
  the end: `..<` stops before it, `..=` yields it without stepping
  past it (so `0..=255u8` never computes `255 + 1`). An inverted or
  empty range yields nothing. A range with no start has no first
  value, so naming a cursor for one fails; a range with no end
  yields until the loop stops it. Stepping lives on the cursor,
  never on the interval.
- `for` names the cursor through `into_iter` and consumes it as an
  `Iterator` (see `control.md`). Index and slice APIs accept bound
  data, never iterators.

