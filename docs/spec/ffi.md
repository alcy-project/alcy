# FFI, Unsafe, and Platform Boundaries (reserved, post-MVP)

Nothing in this chapter is usable in MVP. It reserves decision space
so MVP designs do not foreclose the baremetal story.

## External functions

- `extern "C" { ... }` blocks will declare bodyless functions.
  `"C"` is the only planned convention; other convention strings are
  reserved but undefined. Calling an `extern "C"` function will
  require `unsafe`.

## Unsafe

- `unsafe { }` blocks and `unsafe fn` are reserved. Safe code MUST
  NOT be able to cause undefined behavior; that guarantee is load
  bearing for the whole specification.
- Candidate unsafe operations (to finalize with the unsafe design):
  raw pointer dereference, union field reads, volatile/MMIO access,
  and `extern "C"` calls. Raw pointer types themselves are MVP-excluded.

## Layout and statics

- Default struct layout is compiler-chosen. `#[repr(C)]` is the
  reserved form for FFI-shared structs; the attribute system as a
  whole arrives later. C fixed-width integer types are intended to be
  bit-compatible with alcy integer types.
- `static` items MUST NOT contain `&mut`. Shared references to static
  storage are permitted. Mutable statics do not exist in MVP and
  remain under careful review thereafter.

## Mangling

- alcy-convention symbols mangle toward
  `alcy_<package>_<module path>_<name>`; details finalize with
  package artifacts. `extern "C"` names are unmangled.
