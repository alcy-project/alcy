# Types (MVP)

## Numeric tower

- Integers `i8`-`i64`, `u8`-`u64`, `isize`/`usize`; floats `f32`/`f64`;
  `bool`. `f16`, 128-bit integers, posits, and decimals are deferred.
- Literals: decimal, `0b`/`0o`/`0x`, type suffixes (`42i32`, `1.5f64`),
  `_` separators.
- Overflow: the source arithmetic is checked when the build's
  `overflow-checks` key is on (`debug` says on, `release` off, and
  `--overflow-checks=on|off` overrides). Checked, an overflow in
  `+`, `-`, and `*` panics, an overshift panics, and a signed
  `MIN / -1` panics; unchecked, `+ - *` wrap, a shift count rides
  modulo the type's width, and `MIN / -1` wraps. Division or
  remainder by zero always panics, `MIN % -1` is zero, and no
  arithmetic operation is undefined. The policy belongs to the build,
  not to the module (ADR-0065, ADR-0068), so one lowered program
  serves every profile; `wrapping_add`, `wrapping_sub`, and
  `wrapping_mul` name the wrapping forms for code that wants them in
  every profile. A constant expression is evaluated at compile time
  and never follows the profile: an overflow in `+`, `-`, and `*` and
  a zero divisor are errors there, while an overshift and a signed
  `MIN / -1` keep the defined results above (ADR-0065).
- Casts: a cast between a float and an integer truncates toward zero,
  and a value the destination cannot hold saturates - its minimum below
  the destination's range and its maximum above it, with NaN becoming
  zero (ADR-0077). A cast is never undefined and never traps.

## Tuples

- Tuple types `(T, U)` are structural and concrete (no polymorphism
  in MVP): construction, `.0` access, and destructuring. The unit
  type `()` is the empty tuple.

## Fixed arrays

- Array types `[T; N]` are fixed-size and homogeneous; `N` is a
  decimal length. Literals are lists (`[a, b]`) or repeats (`[e; N]`,
  evaluated once); empty literals are rejected.
- Indexing reads and writes through places with panic-on-out-of-bounds
  semantics. Arrays are `Copy` if and only if their element is. The
  length is a decimal literal, a value parameter in scope (`[T; N]`
  inside a declaration that spells `N: usize`), or a path naming a
  `const` integer value, whose initializer is evaluated at compile
  time (see `comp.md`).

## Sizes

- Every value's size is known at compile time, and a type without a
  size has no values: it appears only as a borrow's pointee (ADR-0083).
  A parameter, return, local, field, array element, and type argument
  is a sized type, so layout, moves, and ABI lowering are total over
  everything a function can name.
- The unsized types are `[T]` and a future trait object. Each appears
  only as the pointee of `&`/`&mut`, and a borrow of one is a fat,
  sized value; a thin raw pointer cannot name one.
- `str` is a sized, compiler-known view: a pointer and a length travel
  together as the value, and it is not a special case of `[T]`.
- Sizedness is an invariant, not a bound. No declaration spells it,
  nothing relaxes it, and there is no negative form; the compiler
  derives a type's size from its shape the way it derives `Copy`.
- A type argument is sized: a slice or a trait object is never a type
  argument, and generic code that wants one spells a borrow (`&[T]`).
- There is no owned unsized value: `Box<[T]>`, `Box<str>`, and a
  future `Box<dyn>` have no spelling. The types that own
  runtime-length data are `Vec<T>` and `String`.

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

## Indexing over specs

- `a[i]` over a nominal type resolves through the sealed `Index` spec:
  the implementation picks the index and element types, the element is
  the value the expression names, and a miss panics the way array
  indexing does. `a[i] = v` and `&mut a[i]` resolve through
  `IndexMut`; `&a[i]` reads through `Index`. Arrays and slices keep
  their builtin indexing, and a nominal type with no implementation is
  not indexable. See `docs/adr/0053-operators-are-sealed-specs.md`.
- The standard containers implement the pair: `Vec<T>` with `usize`
  and `T`, `String` with `usize` and `u8`, and `Map<V>` with `str` and
  `V`. The specs are sealed to the `alcy/std` suite, so the operator
  covers these types until a package lists the spec in its own
  `[spec] implement` (see `modules.md`).

## Equality over specs

- `==` over a nominal type resolves through the sealed `PartialEq`
  spec, and `!=` is its negation; a type with no implementation has no
  equality. Scalars and addresses keep their builtin comparison, and a
  tuple or array compares element-wise when every element is itself
  builtin comparable. See
  `docs/adr/0053-operators-are-sealed-specs.md`.
- `Eq` is the reflexivity marker over `PartialEq` and declares no
  method of its own: implementing it requires a `PartialEq`
  implementation for the same type. `String` implements both; floats
  implement only `PartialEq`, because IEEE equality is not reflexive.
- `Vec<T>` compares lengths and then elements through `PartialEq` in
  turn, and `String` compares its bytes. The element's capability is
  checked where the generic body is instantiated until spec bounds
  land (see `deferred.md`), so comparing a vector of a type without
  `PartialEq` is an error at that use rather than at the
  implementation.

## Comparison over specs

- `<`, `<=`, `>`, and `>=` over a nominal type resolve through the
  sealed `PartialOrd` spec: `fn partial_cmp(self: &Self, other: &Self)
  -> Option<Ordering>`. A `None` - an incomparable pair, a NaN below
  the surface - makes all four comparisons false; a `Some` compares
  the `Ordering` it carries. Scalars keep their builtin comparison.
- `Ordering` is `core`'s three-variant enum (`Less`, `Equal`,
  `Greater`), and `Ord` is the total-order spec over `PartialOrd`:
  `fn cmp(self: &Self, other: &Self) -> Ordering` never answers
  "incomparable", and implementing it requires a `PartialOrd`
  implementation for the same type (ADR-0053).
- The seal works like the other operators': the standard library
  implements the specs, and a package outside the suite lists the ones
  it implements in `[spec] implement = [...]` (see `modules.md`).

## Bounds over specs

- A type parameter takes bounds where it is declared - on free
  functions, methods, spec methods, and impl blocks, never on structs
  or enums: `fn show_all<T: Display + PartialEq>(items: &[T])`. Inside
  the body `T` answers the bound specs' methods, and a call
  instantiates only when each argument's type implements each bound.
  The refusal names the caller's type at the call rather than a line
  inside the library, which is where an unbounded body's failure used
  to surface. Multiple bounds join with `+`; `where` stays reserved.
  See `docs/adr/0028-spec-system.md`.
- An annotation is a value parameter's type when it is a single
  primitive integer (`N: usize`); every other annotation is a bound,
  and a declaration validates that each one names a spec.
- An implementation of a refined spec carries its supers, so
  `impl Eq for T` satisfies a `PartialEq` bound.
- A parameter that no argument binds but a bound mentions is inferred
  from the bound (`fn collect<I: Iterator<T>, T>(iter: I) -> Vec<T>`):
  the first declared method's signature says where the spec's argument
  appears (`next` returns `Option<T>`), and the implementation the
  bound-covered parameter has carries its value. A parameter nested
  under one nominal application infers from the instance the argument
  names (`&Vec<T>` binds from `&Vec<i32>`), reading each level's
  argument in turn; an argument that names no instance leaves the call
  to name the turbofish it needs.
- `Display` accepts the builtin convertibles (`i8`-`u64`, `f32`/`f64`,
  `bool`, `str`) without an implementation, because `{}` formats them
  with no impl at all.

## Arithmetic over specs

- `a + b`, `a - b`, `a * b`, `a / b`, `a % b`, and `-a` over a
  nominal type resolve through the sealed specs in `core`: `Add`,
  `Sub`, `Mul`, `Div`, `Rem`, and `Neg`. Every method takes its
  operands by value (`fn add(self: Self, other: Self) -> Self`), so an
  operator consumes them, which for a `Copy` type reads as a copy.
  Scalars keep their builtin arithmetic, and a nominal type with no
  implementation has no operator. See
  `docs/adr/0053-operators-are-sealed-specs.md`.
- `x op= y` runs the binary spec and stores its result back into the
  place, so a nominal place reaches `Add` and its siblings through the
  same implementation.
- The seal works like the other operators': the standard library
  implements them, and a package outside the suite lists the ones it
  implements in `[spec] implement = [...]` (see `modules.md`).

## Text (staged)

- The compiler-known text type is `str`: byte sequences backing
  string literals. Nothing validates them - not a literal, not a
  `String`'s byte API, not a foreign function's bytes - so every
  operation on `str` is total over arbitrary bytes (ADR-0091).
- `len_bytes` is the byte count. `count_chars` counts scalar values,
  where an ill-formed subsequence counts as one replacement unit, the
  way Unicode's decoding algorithms consume the maximal subpart of
  one; `count_graphemes` arrives with the segmentation tables.
- Whether bytes are valid UTF-8 is a query - `unicode`'s
  `utf8::valid` - and a `char` comes only from a checked decode.
  Validating literals is a strictness decision of its own and is
  reserved.
- `char` (a Unicode scalar value), grapheme segmentation, and width
  live post-MVP; the names are reserved. Baremetal targets without
  core use `u8`/`u32` directly.

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
- `(0..<10).step_by(2)` names a cursor advancing two values at a
  time: 0, 2, 4, 6, 8. The stride must be positive; zero — or a
  negative one — fails rather than yielding one value forever. An
  inclusive end hit exactly is yielded and ends the run; a stride
  that would pass the end stops before it instead, so no step after
  the last value ever wraps. The rules above apply unchanged: an
  inverted range with any stride yields nothing, and an open end
  strides until the loop stops it.
- `for` names the cursor through `into_iter` and consumes it as an
  `Iterator` (see `control.md`). Index and slice APIs accept bound
  data, never iterators.
