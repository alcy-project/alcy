# Deferred Features (post-MVP, unordered)

Tracked here so MVP decisions stay compatible. None of these may be
relied upon by MVP programs or by the MVP compiler implementation.

## Language

- User-defined generics and generic functions; `spec` (trait) definitions
  and dispatch, coherence rules, monomorphization strategy.
- Closures and trait objects; higher-ranked region polymorphism beyond
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

`async`, `await`, `union`, `register`, `comp`, `extern`,
`unsafe`, `for`, `in`, `where`, `dyn`. The MVP keyword set is
frozen in `keywords.md`; additions require a specification update.
