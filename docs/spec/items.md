# Items (MVP)

## Bindings

- Declarations use `:=`, which always introduces bindings:
  `x := expr`, `mut x := expr`, `x: T := expr`, `mut x: T := expr`.
- Reassignment uses `=` which MUST refer to an existing `mut` binding.
- A `:=` in one scope MUST introduce at least one new binding;
  otherwise it is a compile-time error suggesting `=`.
- Declaration left-hand sides share the pattern grammar with `match`
  (see `control.md`): `(c, _) := ...` destructures, `_ := ...`
  explicitly discards a value (silencing `must_use`; see `control.md`).

## Functions and associated items

- `fn` declares free functions. Signatures carry explicit types;
  bodies infer locals intraprocedurally (integer literals default to
  `i32`, float literals to `f64`).
- A function with a non-`()` return type must end every path of its
  body in `ret` or a diverging expression (`panic`, an `if`/`match`
  whose branches all diverge); the body's tail value is not the
  return value (`control.md`). A `comp fn` keeps its tail-`ret` form
  (`comp.md`).
- Inherent `impl` blocks are MVP. A parameter list (`impl<T> ...`)
  makes the block generic; its methods are checked and specialized per
  receiver instantiation.
- A `spec` declares a capability as method signatures, and
  `impl S for T` opts one type in. Specs live in the type namespace:
  `use` imports them and a prelude facade re-exports core's, which is
  what puts a spec's methods on a call — dispatch tries inherent
  methods first, then the in-scope spec impls. Coherence is global
  (one impl per spec and type), and every method the spec declares
  must be implemented with a matching signature. An implementation
  lives where the type does or where the spec does (ADR-0090): `impl
  Spec for Type` is accepted in the package that declares `Type` or
  the package that declares `Spec`, and an inherent block only in the
  type's package. There are no covered types, so `impl MySpec for
  Vec<MyType>` is accepted only where `MySpec` is declared. A builtin
  type (`str`, a slice, an array, a scalar) belongs to the staged
  suite, and only its packages implement for one. See `grammar.md`.
- A spec may be sealed to its suite: the declaring package's manifest
  names it in `[spec] suite-only`, and only that package or a package
  of its suite may then implement it. A spec the list does not name
  stays open, and an outside implementation is an error until the
  implementing-side manifest key ships. See `modules.md`.
- A spec may name a super-spec (`spec Eq: PartialEq`): every
  implementation of the spec requires an implementation of the super
  for the same target shape, the super must resolve to a spec in
  scope, and the chain must not cycle. A generic super-spec is
  deferred, as is a bound admitting the super's methods.
- `static` items have storage and MUST NOT contain `&mut`.
  `const X: T = ...` items are compile-time values: the initializer is
  evaluated during compilation, must terminate within the evaluation
  bound, and is pure (no runtime storage or I/O). A const value is
  immutable and readable wherever a value is; `const x := expr` in
  declaration position is the same thing for a local. See `comp.md`.

## Intrinsic declarations (Bootstrap)

- `intrinsic fn name(params) (-> ret)?;` declares a compiler-provided
  function: a signature without a body, terminated by `;`. An
  `unsafe` prefix states a precondition the compiler cannot check,
  and the checker verifies it against the set below (`ffi.md`). Only
  free functions may be intrinsic; `intrinsic` methods are rejected.
- The compiler knows a closed set, enumerated here. Declaring any
  other name is a compile-time error:
  - `memcopy<T>(dst: &mut T, src: &T, n: usize)` copies `n` bytes
    between two buffers of one storage type.
  - `panic(msg: str)` diverges through the runtime abort.
  - `sys_write(fd: i32, buf: str) -> i64` and
    `sys_read(fd: i32, out: &mut u8, out_len: usize) -> i64` are one
    `write(2)`/`read(2)`: the byte count, or the negated error number
    the call reported. A short write and an interrupted call are the
    caller's to see; the std wraps them with `sys`'s `Fd` and
    `OsError`, and the write-all loop stays with the print hooks
    (ADR-0117).
  - `str_len(s: str) -> usize`, `str_byte(s: str, i: usize) -> u8`,
    and `str_slice(s: str, start: usize, end: usize) -> str` are the
    string primitives. Out-of-bounds `str_byte`/`str_slice` panic.
  - `str_from_parts(ptr: &u8, len: usize) -> str` builds a view over
    caller-provided bytes. Only core uses it, to expose `String` as
    `str`; arbitrary pointers are the caller's responsibility.
  - `slice_len<T>(s: &[T]) -> usize` reports a slice view's element
    count.
  - `slice_from_parts<T>(ptr: &T, len: usize) -> &[T]` builds a shared
    view over caller-provided elements, and `slice_from_parts_mut<T>`
    is its exclusive counterpart, taking and returning `&mut`. As
    with `str_from_parts`, arbitrary pointers are the caller's
    responsibility; the view keeps the loan of the referent it was
    built from.
  - `alloc<T>(count: usize) -> &mut MaybeUninit<T>` reserves room for
    `count` elements of `T` at `T`'s own size and alignment, and
    returns the unique owning reference. A zero `count` still yields a
    distinct, freeable pointer. A null result on allocation failure is
    a runtime condition the caller must handle.
  - `dealloc<T>(ptr: &mut MaybeUninit<T>, count: usize)` releases a
    block, consuming the reference. `count` must match the value passed
    to `alloc`. There is no garbage collector; dropping an owned
    reference is not a runtime operation, so a leaked block leaks.
  - `size_of<T>() -> usize` and `align_of<T>() -> usize` report the
    allocation size and the required alignment of `T`.
  - `wrapping_add<T>(a: T, b: T) -> T`, `wrapping_sub<T>`, and
    `wrapping_mul<T>` name the wrapping forms of the source
    arithmetic: the result is the mathematical one reduced modulo 2^N
    in every build. `T` is an integer type; a hash that relies on the
    wrap says so through these (ADR-0065).
  - `elem_ptr<T>(ptr: &mut MaybeUninit<T>, index: usize) -> &mut
    MaybeUninit<T>` offsets a pointer by whole elements. It is
    unchecked: the result is in bounds exactly when the caller keeps
    `index` within the buffer's length, so the growable containers
    perform the bounds check before calling it.
  - `uninit_write<T>(slot: &mut MaybeUninit<T>, value: T)` moves a
    value into an unwritten slot, leaving the slot initialized.
  - `uninit_assume<T>(slot: &mut MaybeUninit<T>) -> &mut T` releases a
    slot as a mutable reference to its value. Reading through the
    result before anything was written yields whatever the allocator
    returned. See `docs/adr/0027-maybe-uninit-storage.md`.
  - `ptr_offset<T>(ptr: *T, count: isize) -> *T` offsets a raw
    pointer by whole elements; `ptr_offset_mut<T>` is its exclusive
    counterpart over `*mut T`. The count is signed, so one spelling
    moves both ways. The result is in bounds exactly when the caller
    keeps it within the buffer the pointer names.
  - The allocation intrinsics are generic. `elem_ptr`, `dealloc`,
    `uninit_write`, and `uninit_assume` recover `T` from the pointee of
    their reference argument, so a call admits one instantiation;
    `wrapping_add`, `wrapping_sub`, and `wrapping_mul` recover it from
    their arguments; `alloc`, `size_of`, and `align_of` have no
    argument to recover it from and take `T` from a turbofish. A
    generic intrinsic's declared shape is still checked against the
    canonical one, with each type parameter bound to a placeholder.
  - `&u8` and `&mut u8` are not indexable. Element access through a
    heap pointer arrives with the growable containers, which own the
    bounds check. See `docs/adr/0010-typed-heap-primitives.md`.
- `memcopy`, `str_from_parts`, `slice_from_parts`,
  `slice_from_parts_mut`, `alloc`, `dealloc`, `elem_ptr`, `elem_ref`,
  `uninit_assume`, `ptr_offset`, `ptr_offset_mut`, `sys_read`,
  `file_size`, `read_file_into`, `write_file_all`, `arg_count`, and
  `arg` carry a precondition the compiler cannot check, so their
  declarations say `unsafe` and a call needs an `unsafe` block
  (`ffi.md`). The checker verifies the marker against the set the way
  it verifies the shape, so leaving it out, or adding it to a safe
  intrinsic, is an error. The remaining intrinsics stay safe.
- `print(comp fmt: str, args: ..)` and
  `println(comp fmt: str, args: ..)` are `io`'s declarations,
  compiler-expanded like `fmt::format`: the format string is
  comp-known and the call's remaining arguments are collected at
  compile time (`fmt.md`, ADR-0055). The declared bodies never run.
- Calls to intrinsics check like ordinary calls. Intrinsics have no
  bodies to lower, borrow, or specialize; `comp` parameters on
  intrinsics are rejected.
- An intrinsic may be generic. Generic intrinsics register one
  signature per instantiation, resolved at their call sites.

## Linkable symbols

- Every symbol the compiler defines is derived from the signature it
  names: the module path, the item name, what the item is (free
  function, associated function, or method), and the type arguments of
  the instantiation. A source name never reaches the linker, so no
  program can collide with a C library entry point or with another
  alcy item. See `docs/adr/0011-symbol-mangling.md`.
- The encoding is deterministic: the same signature yields the same
  symbol in any lowering order, so a profiler, a debugger, and a
  backtrace all read the same name.
- A C entry point is not encoded. It keeps the name it was declared
  with, because it is not ours to rewrite.

## Structs and enums

- Structs have named fields only and no constructors.
  Construction initializes every field; `..base` move-update is
  allowed. A field without `pub` is private to the module that
  declares the struct, which gates reads, writes, construction, and
  destructuring alike (`modules.md`). A type may declare
  `fn drop(self: Self)`; scope exit and
  `return` run it, and a type without one ends its destructible fields.
  See `values.md`.
- Enums have unit and tuple variants only (`Ok(T)`/`Err(E)` and
  `Some(T)`/`None` are the canonical examples). Struct variants and
  explicit discriminants are deferred; layout is the compiler's except
  for the niches promised below.

## Layout

- The language promises two niches (ADR-0087): a reference is never
  null, and a `bool` is zero or one. An option-shaped enum over a
  reference to a sized type, or over `bool`, therefore has the payload's
  size and alignment: `Option<&i32>` is a pointer and `Option<bool>` is
  a byte. A reference to a slice is fat - a data pointer and a length -
  and a niche there is not promised: an enum over one carries a
  discriminant like any other enum.
- Nothing else is promised. Field order, an enum's discriminant width
  and placement, a zero-sized type's treatment, and padding are the
  compiler's, and `size_of` and `align_of` are a function of the type
  for one compiler version and target, not across them.
- The only stable boundary is a declaration in the C convention
  (`extern "C"`), whose scalar parameters and returns map to the
  platform's C ABI. A struct or enum crossing that boundary waits for
  `repr(C)`, and the internal calling convention is not a promise.
