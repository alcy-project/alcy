# Deferred Features (post-MVP, unordered)

Tracked here so MVP decisions stay compatible. None of these may be
relied upon by MVP programs or by the MVP compiler implementation.

## Language

- Destructor glue reaches a struct's or tuple's fields, an enum's live
  variant after a discriminant read, and an array's elements in a loop.
  A move out of a type with its own `drop` is refused by design, and
  `Vec::grow` moves elements rather than ending them. See
  `docs/adr/0013-destructors-consume-their-value.md`.
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
  is solved. Region-annotated captures solve it by inference, with no
  written lifetimes; see `docs/adr/0076-region-annotated-captures.md`.
  Each rule is stated as a case the
  conformance suite runs. See `docs/adr/0012-reborrow-on-reference-read.md`.
- `spec` (trait) bounds on type parameters, `where` clauses, and
  monomorphization beyond today's per-instantiation interning (enums,
  structs, free functions, methods, and spec methods where the impl is
  generic). Declarations, implementations,
  dispatch, and coherence are settled (see `items.md` and
  `grammar.md`); a bound admitting a super-spec's methods, and generic
  super-specs, arrive with this work.
- The implementing-side manifest key that would let a package outside a
  seal opt in to a suite-only spec; seals are one-way until it ships
  (see `modules.md`).
- The owning environment for closures: capturing a non-Copy value
  by value, and a capturing closure escaping the frame that built
  it. Until it lands a capture is a reference or a copied value,
  and a closure that captures borrows its frame. Spec objects;
  higher-ranked region polymorphism beyond struct projection.
- `unsafe` methods (receiver or spec methods), unsafe function types
  for callbacks, `union` and volatile access, inline assembly, and an
  erased `void*` spelling. The gate, its precondition-carrying
  intrinsics, raw pointers, and `extern "C"` declarations are
  Bootstrap; see `ffi.md`.
- String literal patterns, slice patterns.
- Struct variants for enums; tuple struct declarations.
- `async`, parallel constructs, `union` types, `register` operations.
- `From`-style error conversion; `Debug` printing; the combinators
  (`map`, `and_then`, `or`) on `Result` (they landed on
  `Option`).
- `break` with a value; `main` returning richer types.
- `pub(...)` restricted visibility; glob imports.
- `f16`, 128-bit integers, posit, and decimal types; `char`/`Ascii`/
  grapheme semantics in core (see `types.md`).
- Interior mutability; mutable statics; `const`-position extensions.
- Attribute system in full (`#[repr(C)]` and beyond).

## Toolchain

- The `alcy/std` suite is selected per manifest and staged in pieces
  (`docs/adr/0016-suites-and-the-std-split.md`): `[dependencies]` names members or the whole suite, the
  closure is required rather than pulled in, and only the selected
  members become prelude facades. What remains is `[modules] export`
  enforcement for standard-library members, cross-package `use`
  between members, and the registry or
  git fetchers that every other owner and suite needs — a specifier
  naming a source the compiler cannot fetch is an explicit error, not a
  silent skip.
- Summary-carrying package artifacts and cross-package compilation: a
  `[lib]` target builds to a plain object until then.
- Custom linker, incremental compilation and linking.
- Parallel compilation engine with demand-driven summaries.
- Refinement types over a decidable predicate fragment.
- Language server, formatter, linter, and other surrounding tools
  (see the bootstrap vision in `../../src/docs/architecture.md`).

## Keywords reserved for the above

`async`, `await`, `union`, `register`,
`where`, `dyn`. The MVP keyword set is
frozen in `keywords.md`; additions require a specification update.
