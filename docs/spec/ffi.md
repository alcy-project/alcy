# FFI, Unsafe, and Platform Boundaries

The `unsafe` gate is Bootstrap, described here as it works today.
The rest of the chapter is reserved: nothing else in it is usable
yet, and it is kept so MVP designs do not foreclose the baremetal
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
- The precondition-carrying intrinsics are declared `unsafe` —
  `memcopy`, `str_from_parts`, `slice_from_parts`,
  `slice_from_parts_mut`, `alloc`, `dealloc`, `elem_ptr`, `elem_ref`,
  and `uninit_assume` — and the checker verifies the marker against
  the canonical set the way it verifies the shape (`items.md`), so
  the declaration and the gate cannot drift. `uninit_write` and
  `uninit_ref` stay safe: they are the safe half of `MaybeUninit`.
  Byte access, `size_of`, `align_of`, `print`, `println`, `panic`,
  and `sys_write` stay safe too.
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

## Raw pointers (reserved)

- `*T` and `*mut T` will be thin, Copy, and outside the region
  system. Creating one will be safe: `&x as *T` and `p as *mut U`,
  with integers crossing by cast too, which is what makes `0 as *T`
  the null pointer. Dereferencing and offsetting will be unsafe
  operations.
- C's `void*` is expressed as `*u8` and cast until an erased pointer
  earns its own spelling.

## External functions (reserved)

- `extern "C" { ... }` blocks will declare bodyless functions.
  `"C"` is the only planned convention; other convention strings are
  reserved but undefined. Calling an `extern "C"` function will
  require `unsafe`; variadic declarations and by-value aggregates
  wait for the ABI slices.

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
