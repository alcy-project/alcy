# Deferred Features (post-MVP, unordered)

Tracked here so MVP decisions stay compatible. None of these may be
relied upon by MVP programs or by the MVP compiler implementation.

## Language

- Destructor glue reaches a struct's fields, but not an enum variant's
  payload or an array's elements: both need a discriminant read or a
  loop, which the drop emitter does not build. A type reaching a
  destructor that way must declare its own `drop`, and the compiler warns
  when it cannot place one. Moving one field out of a struct also does
  not retire the struct's own destructor; field-level move tracking is
  still to come. See `docs/adr/0013`. For the same reason `Vec::grow`
  moves elements rather than ending them and `Vec::clear` forgets them,
  so a `T` with a destructor leaks until the buffer is released.
- Generic structs, generic enums, generic free functions, and generic
  intrinsics are MVP: they intern per instantiation, and `impl<T>
  Name<T>` methods specialize per instantiation. A generic call takes
  its type arguments from a turbofish or from the argument types;
  inference binds only a parameter a declared parameter type pins on
  its own, so a call whose parameters cannot be recovered that way
  needs the turbofish.
- `String` is still a fixed 256-byte array, so a formatted string longer
  than that panics rather than growing. A growable `String` needs its
  buffer to be uninitialized storage, but `format` is expanded at
  compile time by a hand-written path in the lowerer that builds the
  string value directly, writes the pieces into the `buf` field as an
  inline array, and stores `written` into `len`. Giving `String` a heap
  buffer means that path has to allocate at runtime instead, which
  needs a capacity agreed between core and the expander: either the
  format string is handed its scratch as an argument, so the expander
  reads the size from the call site, or the size becomes a constant both
  sides repeat. The first is the honest one. See `docs/spec/fmt.md`.
- A loan's extent is still the span from its birth to the last use of
  anything derived from it, rather than a region solved for. Reborrowing
  lands: a place reached through a reference is named by a dereference
  step, `&mut T` coerces to `&T` at an argument and at a receiver, and
  `elem_ref` and `uninit_ref` let a buffer be read through a shared
  owner, so `Vec<T>` has a read-only `at`. What is left is the
  non-lexical extent, which is the region solver's input, then the
  return-position elision for a reborrow that escapes through a call.
  See `docs/adr/0012`.
- `spec` (trait) definitions and dispatch, coherence rules, and
  monomorphization beyond per-instantiation enum, struct, and method
  specialization.
- Closures and spec objects; higher-ranked region polymorphism beyond
  struct projection; two-phase borrows.
- `match` guards, string literal patterns.
- An or-pattern nested inside another pattern (`(A | B, x) => ...`),
  which needs the distributive expansion `(A, x) | (B, x)`. The
  top-level form works in `match` arms and in `if`/`while`
  conditions.
- Struct variants for enums; tuple struct declarations.
- `async`, parallel constructs, `union` types, `register` operations.
- `From`-style error conversion; `Debug` printing; combinators
  (`map`, `and_then`) on `Result`/`Option`.
- `break` with a value; `main` returning richer types.
- `pub(...)` restricted visibility; glob imports.
- `f16`, 128-bit integers, posit, and decimal types; `Char`/`Ascii`/
  grapheme semantics in core (see `types.md`).
- `Range` iteration, stepping, and `for` loops (representation and
  endpoint-marking frozen in `types.md`).
- Two-phase borrows, so `v.push(v.len())` resolves the receiver before
  the arguments; see `docs/adr/0012`.
- Interior mutability; mutable statics; `const`-position extensions.
- Attribute system in full (`#[repr(C)]` and beyond).

## Toolchain

- The `alcy/std` suite is laid out and documented (ADR-0016) but the
  compiler does not read it. `dependencies` is still the
  `[dependencies.<name>] path = ...` table and selects nothing, `[modules]
  export` is parsed and never enforced, and the whole suite is injected
  as prelude modules, so a program's implicit surface is currently every
  std item rather than the one its `prelude.al` re-exports. The manifest
  schema change to `dependencies = ["alcy/std"]`, per-package selection,
  export enforcement and cross-package `use` are all still to come.
- A std package is one entry module today, so its facade holds the
  definitions and the split is organizational. The nested prelude tree
  that a real split needs has landed (ADR-0016), but populating it
  segfaults in the attach path, so the split is not done: a staged
  `core/prelude.al` plus `core/mem.al` crashes before analysis rather
  than resolving the facade's `use mem::{...}`. The layout and the
  manifests do not depend on the split.


- Summary-carrying package artifacts (`[lib]` targets, cross-package
  compilation).
- Custom linker, incremental compilation and linking.
- Parallel compilation engine with demand-driven summaries.
- Refinement types over a decidable predicate fragment.
- Language server, formatter, linter, and other surrounding tools
  (see the bootstrap vision in `../architecture.md`).

## Keywords reserved for the above

`async`, `await`, `union`, `register`, `extern`,
`unsafe`, `for`, `in`, `where`, `dyn`. The MVP keyword set is
frozen in `keywords.md`; additions require a specification update.
