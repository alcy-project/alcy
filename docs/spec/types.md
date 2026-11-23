# Types

## Numeric tower

- Integers `i8`-`i64`, `u8`-`u64`, `isize`/`usize`; floats `f32`/`f64`;
  `bool`. `f16`, 128-bit integers, posits, and decimals are deferred.
- `isize` and `usize` are their own types, not spellings of a
  fixed width: a signature declares `usize`, a caller supplies `usize`,
  and a cast is the only spelling of the change. Each carries the
  layout of the target word and its own implementation of a spec.
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
  not to the module, so one lowered program
  serves every profile; `wrapping_add`, `wrapping_sub`, and
  `wrapping_mul` name the wrapping forms for code that wants them in
  every profile. A constant expression is evaluated at compile time
  and never follows the profile: an overflow in `+`, `-`, and `*` and
  a zero divisor are errors there, while an overshift and a signed
  `MIN / -1` keep the defined results above.
- `a ** b` is integer-only, repeated squaring under `*`'s checks: a
  negative exponent panics, `x ** 0` is one (`0 ** 0` included), and a
  float `**` is undefined - the operator is declared for integers only.
- `-x` on a float flips its sign bit, so `-(0.0)` is `-0.0` and a
  NaN keeps its payload; on an integer it is the checked `0 - x` above.
- Casts: between two integers, the answer is the value the bit
  pattern names in the destination - a narrower destination keeps the
  low bits, a wider one extends a signed source by its sign and an
  unsigned source by zeros, and one of the same width reads the pattern
  as it stands, so `(-1 as isize) as usize` is the largest `usize`. A
  cast between a float and an integer truncates toward zero, and a
  value the destination cannot hold saturates - its minimum below
  the destination's range and its maximum above it, with NaN becoming
  zero. A cast is never undefined and never traps, and a
  constant expression answers the value the program would read
  (the arithmetic rules reserve errors for checked arithmetic, not
  for casts).

## Tuples

- Tuple types `(T, U)` are structural and concrete (no polymorphism):
  construction, `.0` access, and destructuring. The unit
  type `()` is the empty tuple.

## Fixed arrays

- Array types `[T; N]` are fixed-size and homogeneous; `N` is a
  decimal length. Literals are lists (`[a, b]`) or repeats (`[e; N]`,
  evaluated once); empty literals are rejected. A repeat fills every
  slot with the one value it evaluated, so the element must be `Copy`:
  a move-only element would reach the array as several owners of one
  value, and each slot would end it.
- Indexing reads and writes through places with panic-on-out-of-bounds
  semantics. Arrays are `Copy` if and only if their element is. The
  length is a decimal literal, a value parameter in scope (`[T; N]`
  inside a declaration that spells `N: usize`), or a path naming a
  `const` integer value, whose initializer is evaluated at compile
  time (see `comp.md`).

## Sizes

- Every value's size is known at compile time, and a type without a
  size has no values: it appears only as a borrow's pointee.
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
- A size fits the target's `isize`: an array is its element's size
  times its length, and an aggregate is its fields laid out in order. A
  shape whose size does not fit is refused, so every size
  the compiler reports is one a pointer can be offset by.
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
  not indexable.
- The standard containers implement the pair: `Vec<T>` with `usize`
  and `T`, `String` with `usize` and `u8`, and `HashMap<K, V>` with
  `K` and `V`. The specs are sealed to the `alcy/std` suite, so the
  operator covers these types until a package lists the spec in its own
  `[spec] implement` (see `modules.md`).

## Equality over specs

- `==` over a nominal type resolves through the sealed `PartialEq`
  spec, and `!=` is its negation; a type with no implementation has no
  equality. Scalars and addresses keep their builtin comparison, and a
  tuple or array compares element-wise when every element is itself
  builtin comparable.
- `Eq` is the reflexivity marker over `PartialEq` and declares no
  method of its own: implementing it requires a `PartialEq`
  implementation for the same type. `String` implements both; floats
  implement only `PartialEq`, because IEEE equality is not reflexive.
- `Vec<T>` compares lengths and then elements through `PartialEq` in
  turn, and `String` compares its bytes. The element's capability is
  the implementation's `PartialEq` bound, checked at each use, and the
  body itself is checked once at its definition: comparing
  a vector of a type without `PartialEq` names that use rather than a
  line inside the implementation.

## Hashing over specs

- `Hash` is a sealed spec in `alcy/std/hash`: `fn hash(self: &Self) ->
  u64` returns a digest of the value. It declares no equality: a
  digest summarizes bytes, and the promise that equal keys hash equal
  belongs to the consumer that uses digests to identify slots.
- `HashMap<K, V>` and `HashSet<T>` ask for `K: Hash + Eq`. The table
  hashes to pick a slot and compares with `Eq` to resolve a probe
  chain; a type with a digest but no equality can be hashed without
  ever being a key.
- `str`, `String`, and the integer widths implement `Hash` in the
  suite. A string hashes the bytes it views, so a `str` and a `String`
  with the same bytes share a digest. Like `Eq`, `Hash` is sealed: a
  package outside the suite opts in by listing it in `[spec]
  implement = [...]` (see `modules.md`); without that opt-in a
  user-defined type cannot be a table key.

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
  implementation for the same type.
- The seal works like the other operators': the standard library
  implements the specs, and a package outside the suite lists the ones
  it implements in `[spec] implement = [...]` (see `modules.md`).

## Bounds over specs

- A type parameter takes bounds where it is declared - on free
  functions, methods, spec methods, specs, and impl blocks, never on
  structs or enums: `fn show_all<T: Display + PartialEq>(items: &[T])`.
  Inside the body `T` answers the bound specs' methods, and a call
  instantiates only when each argument's type implements each bound.
  A spec's own parameters are such a declaration: an implementation
  may leave a bound off, and the call still owes it.
  The refusal names the caller's type at the call rather than a line
  inside the library, which is where an unbounded body's failure used
  to surface. Multiple bounds join with `+`; `where` stays reserved.
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

- `a + b`, `a - b`, `a * b`, `a / b`, `a % b`, `a ** b`, and `-a`
  over a nominal type resolve through the sealed specs in `core`:
  `Add`, `Sub`, `Mul`, `Div`, `Rem`, `Pow`, and `Neg`. Every method takes its
  operands by value (`fn add(self: Self, other: Self) -> Self`), so an
  operator consumes them, which for a `Copy` type reads as a copy.
  Scalars keep their builtin arithmetic, and a nominal type with no
  implementation has no operator.
- `x op= y` runs the binary spec and stores its result back into the
  place, so a nominal place reaches `Add` and its siblings through the
  same implementation.
- The seal works like the other operators': the standard library
  implements them, and a package outside the suite lists the ones it
  implements in `[spec] implement = [...]` (see `modules.md`).

## Text (staged)

- The compiler-known text type is `str`: a view over valid UTF-8
  bytes. A source file is UTF-8 (a byte sequence that does not spell
  one valid scalar is a diagnostic, inside a literal as anywhere
  else), so a literal is well-formed; a `str` taken from bytes comes
  from a checked construction, never from an unchecked one. An
  `unsafe str_from_parts` promises its bytes are well-formed the same
  way an aliasing reference promises exclusivity: a view over anything
  else is undefined behaviour.
- `len_bytes` is the byte count. `count_chars` counts scalar values.
  `count_graphemes` arrives with the segmentation tables.
- Whether bytes are valid UTF-8 is a query over `[u8]`, the way a
  comment parser asks. `is_utf8(bytes: &[u8]) -> bool` is that
  predicate, and `from_utf8(bytes: &[u8]) -> Result<str, Utf8Error>`
  is the checked way in. `Utf8Error` carries `valid_up_to`, the
  offset of the first byte that did not decode. `unicode::utf8::valid`
  delegates to the byte predicate. A `char` comes only from a checked
  decode.
- All upward conversions go through that check. `str` to `&[u8]` and
  to `Path` is free, and so is `String::as_str` / `as_bytes`; bytes to
  `str` or `String`, and `Path` to `str`, are checked. The only raw
  construction is `unsafe str_from_parts`, whose caller promises
  well-formed bytes.
- `str_slice` checks bounds and that both ends lie on scalar
  boundaries, panicking when either fails. `byte_slice` offers the
  same boundary check as `Option<str>` to callers that would rather
  handle a refusal.
- `String` in `text` owns valid UTF-8 bytes. Its position-taking and
  byte-taking mutators `insert_str`, `truncate`, `byte_slice`,
  `split_at`, and `from_slice` answer `Option` when a byte or boundary
  would break the invariant. Appending a `char` needs no check, and
  there is no public append of an arbitrary byte. The raw writers
  `spare_capacity_mut`, `set_len`, and `as_mut_slice` are `unsafe` and
  require the bytes below the recorded length to spell scalars.
- `char` is an ASCII scalar (0x00-0x7F) with a one-byte layout, and
  `rune` is a Unicode scalar value with a four-byte layout. Both are
  compiler-known types. A character literal names a `char` when its
  scalar is ASCII and a `rune` otherwise; in a context that expects a
  `rune`, an ASCII literal is that rune. Neither can be built from a
  wider integer with a cast: construction goes through core's checked
  functions, or its two `unsafe` unchecked intrinsics with a proven
  precondition. Grapheme segmentation and width remain deferred.
- Widening a `char` to a `rune` is spelled `rune_from_char(c)`, the
  named form diagnostics recommend. `(c as rune)` names the same cast
  at the primitive level and stays available for low-level code.

## Paths and C strings (staged)

- `path::Path` owns a path in the platform's own spelling, and `&Path`
  is its view. On non-Windows targets the spelling is UTF-8 bytes; on
  Windows it is well-formed UTF-16 units. `#[when]` gives both targets
  the same public names. Unpaired surrogates (WTF-16) are not
  representable.
- `Path::from_str` takes a `str` without a check. `from_bytes` checks
  UTF-8, then encodes UTF-16 on Windows, so a path always round-trips
  through `to_str` or the byte accessors. A non-UTF-8 path the OS can
  name is not representable today. `as_bytes` borrows the UTF-8 bytes
  on non-Windows targets; `encoded_bytes` copies the platform's bytes
  and yields UTF-8 on Windows. `join`, `file_name`, `parent`, and
  `extension` work on components and answer owned values or `None`.
  The file I/O package takes `&Path` and passes the encoded bytes to
  its intrinsics.
- `ffi::CStr` is a borrowed view over bytes ending in a terminator.
  `CStr::from_ptr` is `unsafe`: the pointer must reach a NUL and the
  memory must stay initialized for the view's life. `as_bytes` stops
  at the first NUL, and `as_str` decodes those bytes through
  `from_utf8`, answering `Result<str, Utf8Error>`.
- `ffi::CString` owns the bytes and one trailing terminator. `new`
  and `from_str` answer `None` when the bytes contain an interior
  NUL. `as_bytes` excludes the terminator, `as_bytes_with_nul`
  includes it, and `as_ptr` is the pointer an extern declaration
  takes.
- `env` answers the platform's own bytes for a name, and `env_str`
  checks them as UTF-8. A variable that is set to no bytes is `Some`
  of an empty vector, not `None`.

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
- The interval can be read without a cursor. `Bound::is_bounded`
  reports whether a side names an endpoint, and `Bound::value` answers
  `Option<T>` with it (`None` for `Unbounded`). `Range::contains`
  tests membership using the same bounds the cursor uses: an included
  start admits its endpoint, an excluded one does not, and an open end
  admits every later value. `Range::is_empty` is the negation of that
  membership test at the first value a started range would yield; an
  open-ended range is never empty, and an inverted one always is.
- Iteration is explicit and separate: core declares `Iterator` as
  the capability, and only integer ranges expose one, through a
  `RangeIter` cursor built by `into_iter`; float ranges have no
  iteration method. The cursor ascends from the start and stops at
  the end: `..<` stops before it, `..=` yields it without stepping
  past it (so `0..=255u8` never computes `255 + 1`). Stopping reads
  the value the next step would take, never the distance between the
  endpoints, so a span wider than the type's own range still runs in
  full: `-128i8..=127i8` yields all 256 values of `i8`. An inverted
  or empty range yields nothing. A range with no start has no first
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
