# ADR-0087: Two niches are specified, and layout is not

- Subject: the language
- Status: Accepted
- Date: 2026-10-23

## Context

`docs/spec/items.md` leaves an enum's layout to the compiler; struct
variants, explicit discriminants, and layout guarantees are deferred,
and `deferred.md` defers the attribute system in full, `#[repr(C)]`
included. `size_of` and `align_of` are intrinsics in `core`, so the
layout of a type is observable by programs today, and a promise about
it would be a language promise.

ADR-0083 fixed that every value is sized. This record says what the
language promises about that size, which is more than Rust does:
layout there is unspecified as a whole, while `Option<&T>`'s one-word
shape is load-bearing in public APIs and is only an implementation
fact, and niche support grew past that shape later. FFI needs
`repr(C)` there for the same reason this record states its boundary.

Two shapes are worth promising because every API depends on them: a
reference is never null, and a `bool` is zero or one. Both leave bit
patterns free, and an enum's discriminant can live in one without a
second word.

## Decision

**The language promises two niches — a reference is never null, and a
`bool` is zero or one — and no layout beyond the sizes those make
observable.**

- An enum's discriminant may occupy a niche of a field: a
  reference-typed payload leaves the all-zero pattern free, and a
  `bool` payload leaves every pattern but zero and one. An
  option-shaped enum over a reference to a sized type, or over `bool`,
  therefore has the payload's size and alignment, and `Option<&i32>` is
  a pointer.
- Nothing else is promised. Field order inside a value, an enum's
  discriminant width and placement, a zero-sized type's treatment, the
  layout of a fat pointer (`&[T]` today, a trait object later) and
  whether an enum over one finds a niche, and padding are the
  compiler's.
  The compiler may use niches beyond the two; none of them is a
  promise, and because none is observable except through `size_of`, one
  may appear or disappear in a later compiler.
- Within one compiler version and target, `size_of` and `align_of` are
  a function of the type. Across versions they are not promised:
  pre-MVP, a layout change is a compiler change, and the promise
  surface deliberately stays small.
- The only stable boundary is a declaration in the C convention
  (`extern "C"`), whose scalar parameters and returns map to the
  platform's C ABI. A struct or enum crossing that boundary waits for
  `repr(C)`; until it lands, no layout may be relied on across
  languages.
- The internal calling convention is not a promise either: it is the
  compiler's, may change between versions, and the language has no
  spelling that reveals it.

## Consequences

- The two shapes are cheap and tested. `Option<&T>` costs a pointer,
  `Option<bool>` costs a byte, and core's own tests pin both, so a
  compiler change that loses a niche fails a test rather than silently
  growing an API.
- Layout stays free otherwise. The compiler may reorder fields, choose
  discriminant widths, and pack padding, which is what lets the
  optimizer and future targets move.
- Cross-language struct passing is explicitly waiting on `repr(C)`,
  and scalar `extern "C"` calls already work without it.
- A program that reads `size_of` gets a stable answer for its compiler
  and target, and none across versions; a program that needs more
  states a `repr`, once one exists.

## Alternatives considered

- **No niches at all.** Rejected: `Option<&T>` becomes two words in
  every API that says "a reference or nothing", and every user pays for
  a simple layout rule with a larger value.
- **Niches as an implementation detail, not a promise** — Rust's
  position. Rejected: the shape is load-bearing in public APIs, an
  unstated shape cannot be tested, and a later change to it would be
  invisible instead of deliberate.
- **Arbitrary or geometric niches.** Rejected: the layout of a type
  becomes a puzzle its author must reason about, the promise surface
  grows with the compiler's cleverness, and the benefit over the two
  shapes here is small.
- **A stable layout promise now.** Rejected: it freezes field order,
  discriminant widths, and padding before the backend and the foreign
  ABI work are done, and pre-MVP is exactly the window in which they
  may still move.
- **`repr(C)` now, for structs across the C boundary.** Not chosen:
  the attribute system is deferred, scalar `extern "C"` calls cover
  the interop that exists, and the struct case belongs with that
  decision.
