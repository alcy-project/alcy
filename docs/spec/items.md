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
- Inherent `impl` blocks are MVP. A parameter list (`impl<T> ...`)
  makes the block generic; its methods are checked and specialized per
  receiver instantiation.
- A `spec` declares a capability as method signatures, and
  `impl S for T` opts one type in. Specs live in the type namespace:
  `use` imports them and a prelude facade re-exports core's, which is
  what puts a spec's methods on a call — dispatch tries inherent
  methods first, then the in-scope spec impls. Coherence is global
  (one impl per spec and type), and every method the spec declares
  must be implemented with a matching signature. See `grammar.md`.
- A spec may be sealed to its suite: the declaring package's manifest
  names it in `[spec] suite-only`, and only that package or a package
  of its suite may then implement it. A spec the list does not name
  stays open, and an outside implementation is an error until the
  implementing-side manifest key ships. See `modules.md`.
- `static` items have storage and MUST NOT contain `&mut`.
  `const X: T = ...` items are inline constants restricted to literal
  expressions in MVP (full const evaluation arrives with `comp fn`,
  post-MVP). Binding-position `const` does not exist.

## Intrinsic declarations (Bootstrap)

- `intrinsic fn name(params) (-> ret)?;` declares a compiler-provided
  function: a signature without a body, terminated by `;`. An
  `unsafe` prefix states a precondition the compiler cannot check,
  and the checker verifies it against the set below (`ffi.md`). Only
  free functions may be intrinsic; `intrinsic` methods are rejected.
- The compiler knows a closed set, enumerated here. Declaring any
  other name is a compile-time error:
  - `memcopy(dst: &mut u8, src: &u8, n: usize)` copies `n` bytes.
  - `panic(msg: str)` diverges through the runtime abort.
  - `sys_write(fd: i32, buf: str)` writes `buf` to the file
    descriptor. Backs the ordinary `print`/`println` below; user
    code cannot name file descriptors portably, so this stays
    intrinsic.
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
    `alloc`, `size_of`, and `align_of` have no argument to recover it
    from and take `T` from a turbofish. A generic intrinsic's declared
    shape is still checked against the canonical one, with each type
    parameter bound to a placeholder.
  - `&u8` and `&mut u8` are not indexable. Element access through a
    heap pointer arrives with the growable containers, which own the
    bounds check. See `docs/adr/0010-typed-heap-primitives.md`.
- `memcopy`, `str_from_parts`, `slice_from_parts`,
  `slice_from_parts_mut`, `alloc`, `dealloc`, `elem_ptr`, `elem_ref`,
  `uninit_assume`, `ptr_offset`, and `ptr_offset_mut` carry a
  precondition the compiler cannot check, so their declarations say
  `unsafe` and a call needs an `unsafe` block (`ffi.md`). The checker
  verifies the marker against the set the way it verifies the shape,
  so leaving it out, or adding it to a safe intrinsic, is an error.
  The remaining intrinsics stay safe.
- `print(msg: str)` and `println(msg: str)` are ordinary core
  functions over `sys_write`. They remain callable with or without
  a declaration: without the prelude, the legacy name-based path
  lowers them directly.
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
  allowed. A type may declare `fn drop(self: Self)`; scope exit and
  `return` run it, and a type without one ends its destructible fields.
  See `values.md`.
- Enums have unit and tuple variants only (`Ok(T)`/`Err(E)` and
  `Some(T)`/`None` are the canonical examples). Struct variants,
  explicit discriminants, and layout guarantees are deferred; default
  layout is compiler-chosen.
