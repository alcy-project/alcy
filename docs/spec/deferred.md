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
  still to come. See `docs/adr/0013-destructors-consume-their-value.md`. For the same reason `Vec::grow`
  moves elements rather than ending them and `Vec::clear` forgets them,
  so a `T` with a destructor leaks until the buffer is released.
- Generic structs, generic enums, generic free functions, and generic
  intrinsics are MVP: they intern per instantiation, and `impl<T>
  Name<T>` methods specialize per instantiation. A generic call takes
  its type arguments from a turbofish or from the argument types;
  inference binds only a parameter a declared parameter type pins on
  its own, so a call whose parameters cannot be recovered that way
  needs the turbofish.
- A loan's extent is a region computed by backward liveness over the
  CFG: a loan is live where a value carrying it is read and wherever a
  successor is live, so sibling branches differ. The place a reborrow
  names is the place the reference points into, not the slot holding
  it, so a reborrow is anchored to the loan it derives from and cannot
  be more exclusive than that loan is. A summary names the place a
  returned reference came from, so a loan into one field does not cover
  the whole argument it was projected from. What is left of the region
  model is the relational part: the outlives constraints between regions
  and the intersection a struct composes by, which today hold because a
  reborrow carries its parent's loans rather than because the relation
  is solved. Each rule is stated as a case the
  conformance suite runs. See `docs/adr/0012-reborrow-on-reference-read.md`.
- `spec` (trait) bounds on type parameters, `where` clauses, and
  monomorphization beyond per-instantiation enum, struct, and method
  specialization. Declarations, implementations, dispatch, and
  coherence are settled (see `items.md` and `grammar.md`).
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
- `Range` iteration, stepping, and `for` loops. `Iterator` itself is
  declared by core; the range cursor and the `for` rule that consumes
  it are not yet implemented. Representation, endpoint-marking, and
  sub-slicing are frozen in `types.md`. Slice patterns wait for the
  pattern work.
- Two-phase borrows, so `v.push(v.len())` resolves the receiver before
  the arguments; see `docs/adr/0012-reborrow-on-reference-read.md`.
- Interior mutability; mutable statics; `const`-position extensions.
- Attribute system in full (`#[repr(C)]` and beyond).

## Toolchain

- The `alcy/std` suite is selected per manifest and staged in pieces
  (`docs/adr/0016-suites-and-the-std-split.md`): `[dependencies]` names members or the whole suite, the
  closure is required rather than pulled in, and only the selected
  members become prelude facades. What remains is `[modules] export`
  enforcement, cross-package `use` between members, and the registry or
  git fetchers that every other owner and suite needs — a specifier
  naming a source the compiler cannot fetch is an explicit error, not a
  silent skip.
- Summary-carrying package artifacts (`[lib]` targets, cross-package
  compilation).
- Custom linker, incremental compilation and linking.
- Parallel compilation engine with demand-driven summaries.
- Refinement types over a decidable predicate fragment.
- Language server, formatter, linter, and other surrounding tools
  (see the bootstrap vision in `../../compiler/docs/architecture.md`).

## Keywords reserved for the above

`async`, `await`, `union`, `register`, `extern`,
`unsafe`, `for`, `in`, `where`, `dyn`. The MVP keyword set is
frozen in `keywords.md`; additions require a specification update.
