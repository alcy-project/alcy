# alcy Language Specification - Overview

Normative keywords (MUST, SHOULD, MAY) follow RFC 2119 throughout
`docs/spec/`. Every section carries a staging label:

- **MVP**: required for the first self-contained compiler milestone.
- **Bootstrap**: post-MVP compiler work toward self-hosting (see
  `comp.md`).
- **Post-MVP**: explicitly out of MVP scope; described only to reserve
  decision space, never as a promise of a particular design.

Performance figures do not belong in this specification. Design goals
involving speed live outside `docs/spec/` and MUST NOT be stated as
checkable contracts here.

## Goals

- A move-only, statically typed systems language scaling from baremetal
  to high-level software, prioritizing low-level control.
- Memory safety without lifetime annotations: region inference with
  function-summary synthesis (see `ownership.md`, `summaries.md`).
- Predictable, explicit semantics: control flow, ownership, and
  non-trivial cost are visible at the use site.
- Rust compatibility of spirit, not of syntax: inherit what works
  (affine ownership, exclusive-or-shared borrows, `Result`-based errors),
  redesign what does not (annotations, shadowing discipline, declaration
  syntax, block values).

## Non-goals for MVP

- Generic structs and generic free functions. Generic enums and
  generic inherent methods are supported and monomorphized per
  instantiation (see `errors.md` and `docs/adr/0009`).
- Spec objects, closures, async, compile-time evaluation beyond
  constant items.
- Unsafe code, raw pointers, FFI (reserved; see `ffi.md`).
- Incremental or parallel compilation (see `deferred.md`).

## Core hypotheses (post-MVP research tracks)

- Lifetime synthesis over a region system (no user annotations),
  beyond the MVP summary subset.
- Refinement types over a decidable predicate fragment.
- Whole-program analysis with per-package emission.

These tracks shape reserved decision space in this specification but
impose no MVP implementation burden.

## Reading guide

Start with `values.md`, then `ownership.md` and `summaries.md`
(the thesis), then `errors.md`. `modules.md` through `control.md`
define the static structure (`grammar.md` is the exact grammar);
`ffi.md`, `deferred.md`, and `keywords.md` record boundaries.
