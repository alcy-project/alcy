# Alcy Language Specification - Overview

This is the language a program is written against, and nothing else. It
states what a conforming implementation does, never how this one does it:
no file of the compiler, no module of it, and no symbol it defines appears
here. Where a rule is observable from a program, that is the rule's
argument for belonging here; a decision about the implementation belongs
in `src/docs/`. The reasoning behind either is recorded in the
implementation's comments and tests.

Normative keywords (MUST, SHOULD, MAY) follow RFC 2119 throughout
`docs/spec/`. This specification states the language as it is meant to
be, and everything it states is implemented before 1.0; what is not
implemented yet is listed in `deferred.md`.

A section marked Bootstrap (`comp.md`, `fmt.md`, `ffi.md`) describes the
path to self-hosting, and one marked staged (`types.md`) is covered in
part. The marker says what a document covers, not what is promised: no
part of this specification becomes a promise before 1.0.

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

## Non-goals

- Spec objects: no dispatch through a table; there is no `dyn`. A
  spec method call resolves to one implementation at compile time.
- One execution model: the blocking one (`thread`, `sync`, `atomic`),
  std-owned. An `async`/`await`, if it lands, carries exactly one
  std executor.
- Macros of any kind: compile-time work is `comp`-marked code,
  checked by the ordinary type system.
- User-defined feature flags and conditional compilation of packages:
  a package has one configuration per target. The manifest's
  dependencies, the target, and the toolchain decide the build, and
  the toolchain selects items by the `#[when]` attribute on a closed
  platform axis (os, arch, freestanding) before compilation. The axis
  is not a general mechanism: it is chosen from a set the toolchain
  owns, a dependency names no value of its own, and one
  graph serves each binary; a Cargo-style boolean axis in which every
  package declares its own flags would make every consumer a different
  program. Optional capability is selected as separate packages, or
  paid for always.
- Unsafe code, raw pointers, FFI (shipped through the Bootstrap path;
  see `ffi.md`). The typed
  heap intrinsics are the one place pointers exist: they are checked
  against the intrinsic set's canonical shapes, and user code reaches
  elements only through a container that owns the bounds check.
- Incremental or parallel compilation (see `deferred.md`).

## Core hypotheses (research tracks)

- Lifetime synthesis over a region system (no user annotations),
  beyond the summary subset described here.
- Refinement types over a decidable predicate fragment.
- Whole-program analysis with per-package emission.

These tracks shape reserved decision space in this specification but
impose no implementation burden.

`../principles.md` states where the language is going and the properties
that get it there. Where two designs are both defensible, the one that
serves more of them wins.

## Reading guide

Start with `values.md`, then `ownership.md` and `summaries.md`
(the thesis), then `errors.md`. `modules.md` through `control.md`
define the static structure (`grammar.md` is the exact grammar);
`ffi.md`, `deferred.md`, and `keywords.md` record boundaries.
