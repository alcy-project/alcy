# Deferred Features (post-MVP, unordered)

Tracked here so MVP decisions stay compatible. None of these may be
relied upon by MVP programs or by the MVP compiler implementation.

## Language

- Generic structs, generic enums, generic free functions, and generic
  intrinsics are MVP: they intern per instantiation, and `impl<T>
  Name<T>` methods specialize per instantiation. A generic call takes
  its type arguments from a turbofish or from the argument types;
  inference binds only a parameter a declared parameter type pins on
  its own, so a call whose parameters cannot be recovered that way
  needs the turbofish.
- `spec` (trait) definitions and dispatch, coherence rules, and
  monomorphization beyond per-instantiation enum, struct, and method
  specialization.
- Closures and spec objects; higher-ranked region polymorphism beyond
  struct projection; two-phase borrows.
- `match` guards, string literal patterns.
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
- Interior mutability; mutable statics; `const`-position extensions.
- Attribute system in full (`#[repr(C)]` and beyond).

## Toolchain

- Summary-carrying package artifacts (`[lib]` targets, cross-package
  compilation).
- Custom linker, incremental compilation and linking.
- Parallel compilation engine with demand-driven summaries.
- Refinement types over a decidable predicate fragment.
- Language server, formatter, linter, and other surrounding tools
  (see the bootstrap vision in `ARCHITECTURE.md`).

## Keywords reserved for the above

`async`, `await`, `union`, `register`, `extern`,
`unsafe`, `for`, `in`, `where`, `dyn`. The MVP keyword set is
frozen in `keywords.md`; additions require a specification update.
