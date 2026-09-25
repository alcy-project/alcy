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

## Text (staged)

- The compiler-known text type is `str`: byte sequences backing
  string literals. No validation is performed in MVP; `Char` and the
  core text library live post-MVP. Baremetal targets without core
  use `u8`/`u32` directly.

## Ranges (representation decided, types deferred)

- A range is interval data, not an iterator: a single
  `Range<T> { start: Bound<T>, end: Bound<T> }` with
  `Bound = Included(T) | Excluded(T) | Unbounded`. The `..=`/`..<`
  markers map directly onto bound constructors; an absent endpoint
  needs no marker.
- Iteration is explicit and separate: only integer ranges expose an
  iterator (via a compiler-blessed implementation); float ranges
  have no iteration method. Stepping lives on the iterator, never
  on the interval.
- `for` over a range desugars through a single documented rule to
  the explicit iterator form. Index and slice APIs accept bound
  data, never iterators.
- Range types, their methods, and `for` loops arrive together,
  post-MVP. The operator tokens and the endpoint-marking rule above
  are frozen now.

