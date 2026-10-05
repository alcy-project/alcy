# ADR-0051: `extern "C"` for a minimal ABI

- Subject: the language
- Status: Accepted
- Date: 2026-10-06

## Context

`docs/spec/ffi.md` reserved external functions. The unsafe gate and
raw pointers have landed (ADR-0050), so the declarations can follow.
The immediate value is libc: a program gains read, write, and
allocation through functions the compiler did not check, and the
freestanding slice will want the same declaration form against a
libc-less link.

The temptation is to model C's ABI in full. That means layout
attributes, variadic calls, by-value aggregates, and calling
conventions before any of them has a caller, and every one of them
is surface the compiler must keep honest afterwards. The systems
programs this language targets mostly pass scalars and pointers
across the boundary and read the rest through them.

## Decision

**Declarations are one item, `extern "C" { ... }`.** A block holds
`fn` signatures with no bodies, and each declared name is the symbol
the linker resolves. `"C"` is the only convention string accepted;
another one is a diagnostic rather than a silently ignored
attribute. The block is one item for visibility: `pub extern "C"`
exports every declaration in it.

**Calling is an operation the gate covers.** Every declared function
is an unsafe function, so a call names `unsafe { ... }` like any
other gated act. There is no separate `unsafe extern` spelling to
write, and an extern function cannot become a value until unsafe
function types land.

**The initial ABI is what systems code passes across the boundary
first.** Parameters and returns may be scalars, raw pointers, and
`()`. Everything else is refused with a diagnostic instead of being
lowered optimistically: no by-value aggregates, no
`str`/slices/references, no closures, no generics, no variadics, and
no `#[repr(C)]` until the attribute system lands. Every integer
width and both float widths cross; a C pointer is a `*T` or
`*mut T`.

**Names are the ABI.** An extern declaration is not mangled: the
declared name is the symbol. An alcy function keeps its derived
symbol (ADR-0011), so the two namespaces meet only where a
declaration asks them to. A declaration whose name collides with an
alcy item is an ordinary duplicate-name error within the package.

## Consequences

A hosted build already links libc, so a declaration is usable the
moment it type-checks; the freestanding slice will resolve the same
symbols another way, because the declaration form does not know who
provides them. The contract is the callee's own: the compiler checks
the shapes it can see and the caller's `unsafe` block carries the
rest.

Deferred, each waiting for its own decision: function pointers for
callbacks (unsafe function types), an erased `void*` spelling,
`repr(C)` structs, C statics, variadic declarations, and exporting
alcy functions to C under unmangled names. Each of those narrows
what `unsafe` has to say at the call site, so none of them belongs
in the first slice.
