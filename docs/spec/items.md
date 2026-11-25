# Items

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
- Inherent `impl` blocks are part of the language. A parameter list
  (`impl<T> ...`) makes the block generic; its methods are checked and
  specialized per receiver instantiation. The target type's arguments
  are what bind those parameters, so a parameter the target never names
  leaves the block with nothing to specialize on and is an error.
- A `spec` declares a capability as method signatures, and
  `impl S for T` opts one type in. An `intrinsic spec` is the
  exception: the compiler owns the capability, no `impl` may name
  it, and a witness is synthesized from the target's structure.
  `core` declares `Copy` (a value's bytes can be copied) and
  `Zeroable` (all-zero bytes spell a valid value) this way. Specs
  live in the type namespace:
  `use` imports them and a prelude facade re-exports core's, which is
  what puts a spec's methods on a call — dispatch tries inherent
  methods first, then the in-scope spec impls. Coherence is global
  (one impl per spec and type), and every method the spec declares
  must be implemented with a matching signature. An implementation
  lives where the type does or where the spec does: `impl
  Spec for Type` is accepted in the package that declares `Type` or
  the package that declares `Spec`, and an inherent block only in the
  type's package. There are no covered types, so `impl MySpec for
  Vec<MyType>` is accepted only where `MySpec` is declared. A builtin
  type (`str`, a slice, an array, a scalar) belongs to the staged
  suite, and only its packages implement for one — in a spec impl or
  an inherent block alike. See `grammar.md`.
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
- `#[test]` on a `fn` marks a test: the tool that runs tests gathers
  those functions and calls each one. A test takes no arguments and
  returns unit; it reports success by returning rather than by a value.
  Existence is a separate question answered by `#[when(testing)]`:
  a function so gated is part of the module only for a test run, while
  a build, a check, and a run leave it out entirely. A `#[when(EXPR)]`
  gate followed by a brace groups items without making a scope; the
  items join the module's own list, so a group is a way to give many
  tests one existence condition. The tool that runs tests enters the
  package through a module of its own making, whose items declare the
  `alcy_test_` prefix and the argument intrinsics they need, so a
  module that declares those names itself is refused by the run, and
  the diagnostic that reports it names the reservation.
- `static` items have storage and MUST NOT contain `&mut`.
  `const X: T := ...` items are compile-time values: the initializer is
  evaluated during compilation, must terminate within the evaluation
  bound, and is pure (no runtime storage or I/O). A const value is
  immutable and readable wherever a value is; `const x := expr` in
  declaration position is the same thing for a local. See `comp.md`.

## The staged suite's `str` APIs

- The standard library's text functions return byte offsets that
  always lie on scalar boundaries - `find`, `rfind`, `contains`,
  `split`, `split_whitespace`, `trim*`, `lines`, and the predicates
  built on them. A slice named by one of those offsets never crosses
  a character. The byte-level operations `byte`, `bytes`,
  `find_byte`, `rfind_byte`, and `count_byte` report raw positions
  that are not necessarily boundaries; `byte_slice` and `split_at`
  are the checked way to turn such a position back into `str`.
- `str_slice` checks both bounds and scalar boundaries and panics on
  a half code point as well as on out-of-bounds. `str_byte` is a
  byte read and only bounds-checks.
- `char` comes only from a checked decode, and the byte-level validity
  predicate lives on `[u8]`.
- `String`'s position-taking and byte-taking operations answer `Option`
  when an operation could break the UTF-8 invariant: `from_slice`,
  `byte_slice`, `split_at`, `insert_str`, and `truncate`. Appending a
  `char` needs no check, because the scalar is already known to be
  ASCII. `spare_capacity_mut`, `set_len`, and `as_mut_slice` are
  `unsafe`; their precondition is that the bytes below the recorded
  length spell scalars.

## Item attributes

The names are a closed set, and each one names its own section or
ruler: `when(EXPR)` on an item gates it on the build descriptor,
`repr(C)` on a `struct` fixes its FFI layout, `layout(...)` names
layout settings, `inline(...)` hints at inlining a function, and `test`
on a `fn` marks the function a test.
Other names are rejected with guidance
naming the known set; they are not read past or remembered silently.
An attribute sits on the line above the item; a field layout attribute sits
on the line above its field. Attributes may repeat, but a repeated layout
setting on one item is refused.

## The `layout` attribute

`#[layout(name = value, ...)]` sets layout values by name. The known names are
`min_align`, `max_align`, and `tag_size`; each alignment value is a power of two
from 1 to 16, and `tag_size` is 1, 2, 4, or 8. `tag_size` may be 1, 2, 4, or 8
and applies only to an `enum`; a smaller value than the variant count needs is
refused.

On a `struct` or `enum`, `min_align` raises the type's alignment and
`max_align` caps it. On a field, `min_align` places the field on a
boundary of at least that size and `max_align` caps the alignment used
for its offset; the field's own type layout is unchanged. A field with
a non-zero size must not have a natural alignment above `max_align`,
so no unaligned access is generated; a zero-sized field is exempt.
`min_align` above `max_align` is refused.

## The `inline` attribute

`#[inline(always)]`, `#[inline(hint)]`, and `#[inline(never)]` ask the
LLVM optimizer to always inline, prefer inlining, or never inline the
function. They apply to functions and methods only. A declaration that
has no LLVM body — a spec method, intrinsic, `extern "C"`, or `comp fn`
— cannot carry one.

The setting is an optimization hint, not a change in meaning: a program
behaves the same under each, though a backtrace can show different
frames. Direct backends do no optimization and ignore the setting.

## The `when` gate

- `#[when(EXPR)]` on an item states a compile-time-known condition over the
  build descriptor: the target plus the invocation mode. If `EXPR` is true the
  item is part of the module; if it is false the item does not exist there: it
  is never resolved, checked, or emitted, and its absence is not diagnosed. The
  gate is evaluated when the file is read, and it is the only mechanism that
  selects items.

- `EXPR` is a boolean expression over the build descriptor —
  `target.os`, `target.arch`, `target.freestanding`, and `testing` —
  with `==`, `!=`, `&&`, `||`, `!`, and parentheses. The descriptor's
  values are the ones the invocation knows, and it knows all of them:

  | key | values |
  | --- | --- |
  | `target.os` | `linux`, `darwin`, `windows`, `wasi`, `unknown` |
  | `target.arch` | `x86_64`, `aarch64`, `riscv64`, `wasm32`, `unknown` |
  | `target.freestanding` | boolean |
  | `testing` | boolean; true only for `alcy test` |

  The `wasi` value is reserved for a `wasm32-wasi` target and is not
  produced by any target the toolchain ships today; the current wasm
  target answers `unknown`, matching what its triple spells. A unit
  test can still select `wasi` for the evaluator with an explicit
  descriptor, so the arm is exercised wherever the suite runs.

  `unknown` is the value for a target the toolchain does not know, so
  an unrecognized target answers the comparisons with `unknown` and
  loses no arm silently. A value outside the known set of the key is a
  diagnostic that lists the set; a value that is a string where a
  boolean belongs, or the reverse, is a diagnostic of its own.

- A `when` gate closed on a name is a promise that the name exists on
  the other platform arms too: one arm per OS, all spelling the same
  signature. An arm the gate excludes completely must not be named
  from anywhere else on that target, which is what makes OS-typed code
  in `sys` and `path` possible without a separate package per OS.

- Multiple `when` attributes on one item are a conjunction: every
  gate must pass. Items inside a `#[when(EXPR)] { ... }` group inherit
  the group's gate as part of that conjunction, and are gone from a
  build where it fails.

- The `use`, `static`, and `const` forms take a gate, so a facade
  may re-export the arm that belongs to this target and drop the
  others. `impl` blocks may gate as well, and so may the methods
  inside them; a `spec` block may gate as one item, and its method
  signatures cannot gate individually.

- `testing` is read from the invocation, not the target: `alcy test`
  answers true, and every other command false. A test function's
  marker is its role; this gate is what keeps it out of other builds.

- Unknown attribute names are rejected with guidance; expressions that
  are not boolean are rejected; comparisons of operands of different
  kinds are rejected. Item gates do not exist on statements or
  expressions, only on items.

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
    `OsError`, and the write-all loop stays with the print hooks.
  - `error_kind(code: i32) -> i32` maps a failed call's reported number to a
    stable kind code (0 = Other, 1 = Interrupted, 2 = WouldBlock, 3 = NotFound,
    4 = PermissionDenied, 5 = BrokenPipe, 6 = InvalidInput). The table is the
    target's own, because the C library's errno numbering differs (Linux's
    EAGAIN is 11, BSD's is 35): the LLVM runtime compares against the target's
    constants, the x86_64 runtime uses the Linux kernel's, the freestanding one
    uses the same, and wasm maps WASI's numbers. `sys::OsError` classifies
    through it.
  - `str_len(s: str) -> usize`, `str_byte(s: str, i: usize) -> u8`,
    and `str_slice(s: str, start: usize, end: usize) -> str` are the
    string primitives. `str_byte` panics out of bounds; `str_slice`
    panics out of bounds or at a half code point.
  - `char_from_u8_unchecked(value: u8) -> char` and
    `rune_from_u32_unchecked(value: u32) -> rune` construct a scalar
    without a check. Their preconditions (ASCII, and a Unicode scalar
    respectively) are the caller's obligation; core wraps both in
    checked functions.
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
    returns the unique owning reference. A request that reserves no
    bytes answers an aligned dangling address without allocating, and
    `dealloc` ignores such a request; neither a zero count nor a
    zero-sized element consumes a heap block. A request the runtime
    cannot meet aborts
    the process with the site-less `panic: allocation failed`,
    so the answer is never null; a size or alignment past
    the largest object the address space can hold is refused the same
    way, before the host allocator is asked.
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
    wrap says so through these.
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
    returned.
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
    bounds check.
- `memcopy`, `str_from_parts`, `slice_from_parts`,
  `slice_from_parts_mut`, `alloc`, `dealloc`, `elem_ptr`, `elem_ref`,
  `uninit_assume`, `ptr_offset`, `ptr_offset_mut`, `sys_read`,
  `file_size`, `read_file_into`, `write_file_all`,
  `write_file_all_bytes`, `arg_count`, and `arg` carry a precondition
  the compiler cannot check, so their
  declarations say `unsafe` and a call needs an `unsafe` block
  (`ffi.md`). The checker verifies the marker against the set the way
  it verifies the shape, so leaving it out, or adding it to a safe
  intrinsic, is an error. The remaining intrinsics stay safe.
- `print(comp fmt: str, args: ..)` and
  `println(comp fmt: str, args: ..)` are `io`'s declarations,
  compiler-expanded like `fmt::format`: the format string is
  comp-known and the call's remaining arguments are collected at
  compile time (`fmt.md`). The declared bodies never run.
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
  alcy item.
- The encoding is deterministic: the same signature yields the same
  symbol in any lowering order, so a profiler, a debugger, and a
  backtrace all read the same name.
- A C entry point is not encoded. It keeps the name it was declared
  with, because it is not ours to rewrite.

## Structs and enums

- Structs have named fields only and no constructors.
  Construction initializes every field; `..base` move-update is
  allowed. A field without `pub` is private to the module that
  declares the struct, which limits reads, writes, construction, and
  destructuring alike (`modules.md`). A type may declare
  `fn drop(self: Self)`; scope exit and
  `return` run it, and a type without one ends its destructible fields.
  See `values.md`.
- Enums have unit and tuple variants only (`Ok(T)`/`Err(E)` and
  `Some(T)`/`None` are the canonical examples). Struct variants and
  explicit discriminants are deferred; layout is the compiler's except
  for the niches promised below.
- An enum's variants and a spec's methods are the item's declaration;
  growth happens by declaring a new type or a new spec, never by
  extending one that existing code matches or implements. Adding a
  variant to an enum is a breaking change for every exhaustive match,
  and adding a method to a spec is one for every implementation.
  A capability that grows is a new spec naming the old one as its
  super-spec (`spec Eq: PartialEq`); a spec whose method set must grow
  is sealed to its suite (`[spec] suite-only`), and an open spec
  promises its set will not grow. There is no `#[non_exhaustive]` and
  no wildcard requirement: a match covering an enum stays exhaustive.

## Layout

- The language promises two niches: a reference is never
  null, and a `bool` is zero or one. An option-shaped enum over a
  reference to a sized type, or over `bool`, therefore has the payload's
  size and alignment: `Option<&i32>` is a pointer and `Option<bool>` is
  a byte. A reference to a slice is fat - a data pointer and a length -
  and a niche there is not promised: an enum over one carries a
  discriminant like any other enum.
- A zero-sized type has `size_of` zero and `align_of` one, unless its
  declaration raises that alignment with `#[layout(min_align = N)]`.
  A zero-length array keeps its element's alignment. The unit,
  the never type, a zero-length array, an empty `struct`, an empty
  `enum`, and an aggregate whose parts are all zero-sized are such
  types. A zero-sized value has no storage of its own: a reference to
  one is an aligned but dangling address, and indexing a zero-sized
  array repeats that address. Construction and destruction still run
  their effects, and `alloc`/`dealloc` for a zero-sized element are
  handled by the runtime without a block, so a `Vec<()>` allocates no
  heap memory.
- An enum's discriminant is the smallest whole byte count that can
  name its variants, unless `#[layout(tag_size = N)]` fixes it. When
  every variant carries only zero-sized payloads, a single-variant
  enum has no discriminant and is zero-sized.
- Nothing else is promised. Field order, an enum's discriminant
  placement, and padding are the compiler's, and `size_of` and
  `align_of` are a function of the type for one compiler version and
  target, not across them.
- The only stable boundary is a declaration in the C convention
  (`extern "C"`), whose scalar parameters and returns map to the
  platform's C ABI. A struct crossing it does so under `#[repr(C)]`,
  which fixes its layout. An enum cannot use that C contract: its
  discriminant's placement is the compiler's choice. `#[layout(tag_size
  = N)]` sets a width in alcy bytes, not a C ABI type.
  The internal calling convention is not a promise.
