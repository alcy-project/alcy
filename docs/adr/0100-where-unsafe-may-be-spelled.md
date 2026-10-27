# ADR-0100: The suite's raw operations live in a closed list

- Subject: the language
- Status: Accepted
- Date: 2026-10-27

## Context

ADR-0050 makes `unsafe` a gate: an operation it covers may appear only
inside `unsafe fn` or `unsafe { ... }`, the gate is lexical, and
everything inside is checked as it was outside. A call to an unsafe
function is itself covered, so the gate marks every call site as well
as every declaration. What ADR-0050 does not say is where the suite
may *declare* the raw operations in the first place, and without that
list "the suite's ordinary code is safe" is a claim nobody can check.

Today the declarations sit in two packages. `core`'s memory module
carries the raw memory and platform surface — `memcopy`, the pointer
offsets, the uninitialized forms, `drop_in_place`, `read`, the
entry-point and file syscalls, argument access — and its slice and
string modules carry the raw views (`slice_at`, `slice_from_parts`,
`str_from_parts`). `alloc`'s heap carries the allocation surface
(`alloc`, `dealloc`, `elem_ptr`, `elem_ref`, `uninit_assume`). Every
other block in the suite is a call to one of those: `Vec`'s growth,
`String`'s storage, `Map`'s slot table, `io`'s buffers, `fmt`'s copy.

## Decision

**A raw operation is declared where the language's own packages own
it, and the declaration sites are a closed list. Everywhere else,
`unsafe` is a call to a declaration, with the invariant it keeps
stated at the call site.**

- Declarations live in `core` (raw memory, raw views, the platform
  entry points) and `alloc` (the heap). The machine operations of a
  future `atomic`, `simd`, or `arch` package and the wrapper around a
  foreign function (`extern "C"`) join the list as the same kind of
  declaration, in the package that names the operation.
- A package outside the list may call a declared operation — the gate
  requires the block — but may not perform a raw operation of its own
  and may not declare a new intrinsic. The block states the invariant
  it keeps (the element is initialized, the index is in range, the
  length is within the capacity), so it is auditable where it is read.
- A new declaration site amends this record. A reviewer who cannot
  name the declaration a block calls, or the invariant it keeps,
  refuses it.
- The list is a review rule, not a compiler rule: the compiler keeps
  enforcing the gate itself (ADR-0050), and the two work together —
  the gate makes every raw call visible, and the list says where the
  raw operations may come from.

## Consequences

- The suite's unsafe surface is a small set of declarations to audit,
  and each call site is checkable against the invariant it cites.
- A container's invariant lives in one place: `Vec`'s capacity and
  length, `String`'s length and buffer, `Map`'s state bytes, each
  maintained by the package that owns the type.
- The cost is stated: a package that wants a hand-rolled fast path
  cannot have one privately. It uses a declared operation or the
  declaration is amended in the open.

## Alternatives considered

- **No list; each block is judged on its own.** Rejected: the suite's
  safety claim would be unverifiable, and an intrinsic wrapper would
  look like a shortcut.
- **A budget per package (a maximum number of blocks).** Rejected: a
  count is noise; the list's question is which operation a block
  performs and which invariant it keeps.
- **Forbidding `unsafe` in the suite entirely.** Rejected: the heap,
  the raw views, and the platform entry points cannot be written
  without it, and a rule with permanent exceptions is a list already.
