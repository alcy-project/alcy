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
- The compiler provides a `print(msg: str)` intrinsic, lowered
  directly to a write syscall. It migrates to an ordinary core
  function once FFI lands.
- `static` items have storage and MUST NOT contain `&mut`.
  `const X: T = ...` items are inline constants restricted to literal
  expressions in MVP (full const evaluation arrives with `comp fn`,
  post-MVP). Binding-position `const` does not exist.

## Intrinsic declarations (Bootstrap)

- `intrinsic fn name(params) (-> ret)?;` declares a compiler-provided
  function: a signature without a body, terminated by `;`. Only free
  functions may be intrinsic; `intrinsic` methods are rejected.
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
  - `alloc<T>(count: usize) -> &mut T` reserves room for `count`
    elements of `T` at `T`'s own size and alignment, and returns the
    unique owning reference. Elements are uninitialized. A zero
    `count` still yields a distinct, freeable pointer. A null result
    on allocation failure is a runtime condition the caller must
    handle.
  - `dealloc<T>(ptr: &mut T, count: usize)` releases a block,
    consuming the reference. `count` must match the value passed to
    `alloc`. There is no garbage collector; dropping an owned
    reference is not a runtime operation, so a leaked block leaks.
  - `size_of<T>() -> usize` and `align_of<T>() -> usize` report the
    allocation size and the required alignment of `T`.
  - `elem_ptr<T>(ptr: &mut T, index: usize) -> &mut T` offsets a
    pointer by whole elements. It is unchecked: the result is in
    bounds exactly when the caller keeps `index` within the buffer's
    length, so the growable containers perform the bounds check before
    calling it.
  - The four allocation intrinsics are generic. `elem_ptr` and
    `dealloc` recover `T` from the pointee of their reference
    argument, so a call admits one instantiation; `alloc`, `size_of`,
    and `align_of` have no argument to recover it from and take `T`
    from a turbofish. A generic intrinsic's declared shape is still
    checked against the canonical one, with each type parameter bound
    to a placeholder.
  - `&u8` and `&mut u8` are not indexable. Element access through a
    heap pointer arrives with the growable containers, which own the
    bounds check.
- `print(msg: str)` and `println(msg: str)` are ordinary core
  functions over `sys_write`. They remain callable with or without
  a declaration: without the prelude, the legacy name-based path
  lowers them directly.
- Calls to intrinsics check like ordinary calls. Intrinsics have no
  bodies to lower, borrow, or specialize; `comp` parameters on
  intrinsics are rejected.
- An intrinsic may be generic. Generic intrinsics register one
  signature per instantiation, resolved at their call sites.

## Structs and enums

- Structs have named fields only and no constructors.
  Construction initializes every field; `..base` move-update is
  allowed. Destructors do not exist in MVP: values end at scope exit
  without running user code (panic path included).
- Enums have unit and tuple variants only (`Ok(T)`/`Err(E)` and
  `Some(T)`/`None` are the canonical examples). Struct variants,
  explicit discriminants, and layout guarantees are deferred; default
  layout is compiler-chosen.
