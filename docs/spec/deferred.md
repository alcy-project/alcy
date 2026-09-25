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
- Reborrowing, and the coercions that go with it: `&mut T` used where a
  shorter `&mut T` or a `&T` is expected, including as a method
  receiver. Without it a method taking `&self` cannot be called through
  a `&mut` binding. A growable container therefore has no read-only
  element accessor, and its mutable accessors take `&mut Self`. The
  rules are settled and the extent is non-lexical; see
  `docs/adr/0012`.
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
- Two-phase borrows, so `v.push(v.len())` resolves the receiver before
  the arguments; see `docs/adr/0012`.
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
