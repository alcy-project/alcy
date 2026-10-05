# ADR-0050: Unsafe is a gate on operations

- Subject: the language
- Status: Accepted
- Date: 2026-10-06

## Context

The C FFI story needs two things the language does not have: a
way to call code the compiler did not check, and raw pointers.
`docs/spec/ffi.md` reserved both, and `docs/principles.md` fixed
the shape of the answer: unsafe "arrives as a gate on operations
that already exist, not as a new class of escape", because MMIO,
volatile access, and foreign calls cannot be type-checked. The
`uninit_assume` intrinsic was designed with that gate in mind.

The gate is also already needed for the standard library's own
foundations. `alloc`, `dealloc`, `elem_ptr`, `elem_ref`,
`memcopy`, `uninit_assume`, `str_from_parts`, `slice_from_parts`,
and `slice_from_parts_mut` are callable from safe code today,
and every one of them has a precondition the compiler cannot
check: a live allocation, an in-bounds offset, initialized
storage, a range that names a real buffer. The specification's
promise that safe code cannot cause undefined behaviour is not
yet true.

## Decision

**The gate is `unsafe fn` and `unsafe { ... }`.** A call to an
unsafe function, and the pointer operations below, may appear
only lexically inside an `unsafe` block. A block inside a safe
function is where that function discharges the obligations; a
function that passes them on to its callers declares itself
`unsafe fn`. An unsafe function's body is not implicitly an
unsafe context: each operation that needs the gate names it, so
a reader sees which act is unsafe, not merely that the function
is.

**A block is an expression.** `unsafe { ... }` has the value its
block has, and everything inside it is checked as it was outside:
the gate changes what is allowed, never what is inferred,
borrowed, or moved. `unsafe` does not suspend the region, move,
or drop rules; those keep their diagnostics, because the gate
sits on operations and not on checking.

**What the gate covers.** Calls to unsafe functions — free
functions and intrinsic declarations today, `extern "C"`
declarations with the FFI slice — and dereferencing or offsetting
a raw pointer. The precondition-carrying intrinsics are declared
`unsafe` themselves, and the checker verifies each declaration
against its canonical shape, so the declaration and the gate
cannot drift: `alloc`, `dealloc`, `elem_ptr`, `elem_ref`,
`memcopy`, `uninit_assume`, `str_from_parts`, `slice_from_parts`,
and `slice_from_parts_mut`. `uninit_write` and `uninit_ref` stay
safe: they are the safe half of `MaybeUninit`. `size_of`,
`align_of`, the length and byte accessors, `print`, `println`,
`panic`, and `sys_write` also stay safe; none of them can cause
undefined behaviour.

**Raw pointers are `*T` and `*mut T`.** Thin, Copy, and outside
the region system. Creating one is safe: `&x as *T`, `p as *mut
U`, and integers cross by cast too (`n as *T`, `p as usize`),
which is what makes `0 as *T` the null pointer. Dereferencing and
offsetting one are unsafe operations. C's `void*` is expressed as
`*u8` and cast until an erased pointer earns its own spelling.

**The contract.** Safe code MUST NOT be able to cause undefined
behaviour; that guarantee stays load bearing. Unsafe code carries
the obligations the compiler cannot check, and each operation has
exactly one: a dereference names a live, aligned, initialized
object; `&mut` exclusivity holds through raw pointers as through
references; an `extern "C"` call satisfies the callee's C
contract; `uninit_assume` reads storage that was written;
`memcopy` copies between regions that do not overlap. A safe
abstraction built over unsafe operations is where those
obligations are established.

## Consequences

The first slices: the gate itself (syntax, checking, and the
intrinsic migration, with the standard library wrapping its uses
in `unsafe` blocks), then raw pointer types with casts,
dereference, and offset, then `extern "C"` declarations and
system libc calls, then freestanding exports and `_start`.

What it costs is one more word at each boundary and a standard
library that says which of its acts are unsafe, which is the
point: the compiler's trust boundary becomes a line a reader can
find. Deferred on purpose: unsafe receiver or spec methods,
unsafe function types for callbacks, `union` and volatile reads,
inline assembly, and an erased `void*` spelling.
