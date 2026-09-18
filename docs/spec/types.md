# Types (MVP)

## Numeric tower

- Integers `i8`–`i128`, `u8`–`u128`, `isize`/`usize`; floats `f32`/`f64`;
  `bool`. `f16`, posits, and decimals are deferred.
- Literals: decimal, `0b`/`0o`/`0x`, type suffixes (`42i32`, `1.5f64`),
  `_` separators.
- Overflow: debug builds check and panic; release builds wrap.
  Division by zero panics in both modes; overshifts panic in debug
  and mask in release. No independent overflow-check flag exists;
  const evaluation follows the same semantics.

## Tuples

- Tuple types `(T, U)` are structural and concrete (no polymorphism
  in MVP): construction, `.0` access, and destructuring. The unit
  type `()` is the empty tuple.

## Text (staged)

- The compiler-known text type is `str`: validated UTF-8 byte
  sequences, backing string literals. Character semantics (`Char`,
  `Ascii`, graphemes, formatting) live in the core library, which
  resolves `'x'` literals to its `Char` type. Baremetal targets
  without core use `u8`/`u32` directly.

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

