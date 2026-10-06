# FFI, Unsafe, and Platform Boundaries

The `unsafe` gate and `extern "C"` declarations are Bootstrap,
described here as they work today. The rest of the chapter is
reserved: it is kept so MVP designs do not foreclose the baremetal
story.

## Unsafe (Bootstrap)

- The gate is `unsafe fn` and `unsafe { ... }` blocks, and it is
  lexical: an operation it covers may appear only inside such a
  block, and an `unsafe fn` body is not one implicitly, so a reader
  sees which act is unsafe rather than merely that the function is.
  See `docs/adr/0050-unsafe-is-a-gate-on-operations.md`.
- `unsafe { ... }` is an expression whose value is the block's, and
  everything inside is checked as it was outside: the gate changes
  what is allowed, never what is inferred, borrowed, or moved.
- A call to an unsafe function is an operation the gate covers. An
  unsafe function that becomes a value waits for unsafe function
  types, so using one in value position is refused rather than
  silently dropping the gate.
- Dereferencing a raw pointer is an operation the gate covers, for a
  read and for a write. Writing needs a `*mut` pointer and a `mut`
  binding that names it, the same way writing through a reference
  needs a `&mut` and a `mut` binding.
- Offsetting a raw pointer is an operation the gate covers. It is a
  call to `ptr_offset`/`ptr_offset_mut` (`items.md`), not a rule of
  `*`, so the gate is the call gate.
- The precondition-carrying intrinsics are declared `unsafe` —
  `memcopy`, `str_from_parts`, `slice_from_parts`,
  `slice_from_parts_mut`, `alloc`, `dealloc`, `elem_ptr`, `elem_ref`,
  `uninit_assume`, `ptr_offset`, and `ptr_offset_mut` — and the
  checker verifies the marker against the canonical set the way it
  verifies the shape (`items.md`), so the declaration and the gate
  cannot drift. `uninit_write` and `uninit_ref` stay safe: they are
  the safe half of `MaybeUninit`. Byte access, `size_of`, `align_of`,
  `print`, `println`, `panic`, and `sys_write` stay safe too.
- Safe code MUST NOT be able to cause undefined behavior; that
  guarantee stays load bearing. Unsafe code carries the obligations
  the compiler cannot check: a dereference names a live, aligned,
  initialized object; `&mut` exclusivity holds through raw pointers
  as through references; an `extern "C"` call satisfies the callee's
  C contract; `uninit_assume` reads storage that was written. A safe
  abstraction built over unsafe operations is where those
  obligations are established.
- Unsafe receiver and spec methods are not implemented yet: the
  marker parses and is refused where it appears.

## Raw pointers (Bootstrap)

- `*T` and `*mut T` are thin, Copy, and outside the region system: a
  value that carries one carries no loan, and a loan the cast was
  taken through expires at the cast. They are the address without
  the obligation.
- Creating one is safe: `&x as *T`, `&mut x as *mut T`, `p as *mut
  U`, and `&x as usize` all move the address around without reading
  through it. `*mut T` coerces to `*T` the way `&mut T` coerces to
  `&T`, and `as` crosses raw pointer kinds, pointees, and integers in
  both directions, which is what makes `0 as *T` the null pointer.
  `&T` never casts to a `*mut T`, and a raw pointer never casts back
  to a reference: a tracked reference has to be built where its loan
  comes from.
- Dereferencing and offsetting are operations the gate covers. A
  `*mut T` names a writable place; a `*T` reads only, and a write
  through one is rejected before the gate is consulted.
- Two raw pointers of one type compare with `==` and `!=` by
  address; ordering is not defined. An erased `void*` will spell as
  `*u8` and cast until it earns its own spelling.

## External functions (Bootstrap)

- `extern "C" { ... }` declares bodyless functions, and each
  declared name is the symbol the linker resolves: an extern
  declaration is not mangled. `"C"` is the only convention accepted;
  another string is a diagnostic. The block is one item for
  visibility, so `pub extern "C"` exports every declaration in it.
  See `docs/adr/0051-extern-c-for-a-minimal-abi.md`.
- Calling a declared function is an operation the gate covers, so
  the call names `unsafe { ... }` and an extern function is refused
  in value position until unsafe function types land. There is no
  `unsafe extern` spelling.
- The initial ABI is scalars, raw pointers, and `()` as a return.
  Everything else is refused with a diagnostic: by-value aggregates,
  `str`/slices/references, closures, generics, and variadics. The
  callee's own contract is the caller's `unsafe` block to keep.

## Layout and statics

- Default struct layout is compiler-chosen. `#[repr(C)]` is the
  reserved form for FFI-shared structs; the attribute system as a
  whole arrives later. C fixed-width integer types are intended to be
  bit-compatible with alcy integer types.
- `static` items MUST NOT contain `&mut`. Shared references to static
  storage are permitted. Mutable statics do not exist in MVP and
  remain under careful review thereafter.

## Mangling

- alcy-convention symbols carry the per-signature `_A` encoding of
  `docs/adr/0011-symbol-mangling.md`; `extern "C"` names are unmangled.
