# Principles

Where this language is going, and the properties that get it there.

This document governs the *language*. For how the compiler that
implements it is put together, see [ARCHITECTURE.md](ARCHITECTURE.md);
`docs/spec/` holds the specification itself.

`docs/spec/overview.md` states the goals and the non-goals for the MVP.
This file is the longer view, and it is also the tie-breaker: when two
designs are both defensible, the one that serves more of what follows
wins. A change that trades one of these away for convenience has to say
so out loud, because they are the reason the language is worth choosing
over the ones it is being compared against.

Two threads run through it. The first is what a programmer is asked to
believe: that nothing happens behind their back, that misuse is caught
or is impossible to do by accident, and that the compiler's freedom to
infer is not paid for with the programmer's freedom to reason. The
second is where the language has to be usable - bare metal and
operating systems, where addresses are physical and no allocator is
guaranteed - because a language that is only comfortable in user space
is not the language this is for.

## A language whose abstractions cost nothing

A construct that cannot be written in machine code with no residue does
not belong in the language. No hidden allocation, no hidden bounds
check that a later pass cannot remove, no vtable, no reference counting.
Where a check is unavoidable it is either provably cheap or it is
visible at the use site.

Concretely: a generic type is monomorphized, not dispatched; a
`MaybeUninit<T>` wrapper is the payload's layout; an element offset is a
typed GEP; a layout query folds to a constant. If a proposal needs a
runtime structure to express something, the answer is a new primitive
rather than an abstraction over one.

## One way to say a thing

The language should have a single obvious spelling for each concept, and
the cost of a shorter spelling is paid once rather than paid by every
reader later. Two spellings of the same idea are a permanent tax on
everyone who learns the language and a permanent source of
inconsistency in the compiler.

This is why `alloc<T>` counts elements instead of bytes: a byte count is
the second spelling of the same operation, and it is the one that cannot
be type-checked. It is why a symbol comes from a signature rather than
from a name: names collide, and a second naming scheme would not fix
it.

## The programmer can see what the compiler decided

Regions, layout, and initialization are all decisions the programmer may
need to reason about, so the language makes them nameable rather than
opaque. `size_of<T>()` is a call you can write. Uninitialized storage
has a type you can name. A borrow is a loan on a place the programmer
can identify.

The corollary is that where a decision *cannot* be named, the language
should be suspicious of needing it. An operation whose preconditions
the programmer cannot state, and whose violation the compiler cannot
see, belongs in a closed intrinsic set with a documented contract - not
in ordinary syntax.

## Misuse is hard by accident and easy on purpose

The whole argument for `alloc<T>` returning
`&mut MaybeUninit<T>` and for the heap intrinsics being a closed,
signature-checked set: reading an element before writing it is a type
error, not a bug, and the one place a programmer can do it deliberately
is a call whose name says so.

The test for a feature is whether misuse is a compile error, a documented
and greppable call, or neither. "Neither" is a defect.

## Memory safety without annotations

Safety comes from the region system, inferred from use, not from a
lifetime the programmer writes. A world where the compiler tracks
lifetimes cannot infer them, so the programmer writes them, and the
result is the annotation burden this language exists to remove.

Two commitments follow. First, a feature that cannot be expressed
under inference does not ship, because it either reintroduces
annotations or reintroduces `unsafe`. Second, inference quality is not
a convenience: a checker that rejects correct code is a correctness
problem for the programmer, not a limitation to be documented.

## Ownership is affine, and the escape hatches are visible

Every value is used once, or destroyed. This is why `dealloc` is a call
and not a rule, and why `Drop` will be an operation the programmer names
rather than a hidden one. A linear discipline is what makes the
absence of a garbage collector a feature.

Where the discipline is insufficient, the language provides a *visible*
way out. `uninit_assume` is the current example: an ordinary call whose
name is the programmer stating a precondition. It will become `unsafe`
when `unsafe` lands, and it was designed with that in mind - the
discharge point was chosen so the gate has somewhere to go.

## Usable in systems, not only in user space

The target is bare metal and operating-system work: no allocator is
guaranteed, addresses are physical, a syscall is a function call. That
sets several priorities.

- **The absence of an allocator must be expressible.** A heap
  container is the common case, not the only case; fixed-capacity
  inline storage is a first-class answer, not a fallback.
- **Symbols are derived from signatures, not names.** A kernel's
  vocabulary is `write`, `read`, `open`, and `close` (ADR-0011).
- **Layout is queryable.** `size_of` and `align_of` are calls.
- **Control over codegen is a language concern, not a build flag**, so
  that the same source means the same thing in a kernel and a test.

`unsafe` is a consequence of this, not a concession: MMIO, volatile
access, and inline assembly cannot be type-checked, and a language for
this domain needs a way to say so at the call site. It arrives as a
gate on operations that already exist, not as a new class of escape.

## What this rules out

For the record, so that a future change does not relitigate them:

- A garbage collector, or any form of tracing ownership. Affine
  ownership is the mechanism; adding tracing on top would replace the
  cost model rather than extend it.
- Implicit allocation, hidden behind an ergonomic-looking signature.
- A `dyn`-shaped value with reference-counted or garbage-collected
  dispatch. Dispatch is monomorphized or explicit.
- A design where the safe subset cannot express a systems program. If
  safe alcy cannot write a driver, the language has failed at its goal
  regardless of what `unsafe` allows.
