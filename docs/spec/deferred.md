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
  still to come. See `docs/adr/0013`.
- Generic structs, generic enums, generic free functions, and generic
  intrinsics are MVP: they intern per instantiation, and `impl<T>
  Name<T>` methods specialize per instantiation. A generic call takes
  its type arguments from a turbofish or from the argument types;
  inference binds only a parameter a declared parameter type pins on
  its own, so a call whose parameters cannot be recovered that way
  needs the turbofish.
- An enum's payload is reached through a pointer into the frame that
  built it, so returning an enum with a payload and keeping it across
  another call reads a dead frame:

  ```
  enum E { A(i32), B }
  fn mk(x: i32) -> E { ret E::A(x) }
  fn get(e: E) -> i32 {
    ret match e { E::A(v) => v, E::B => 0 }
  }
  fn sink(e: E) { _ := get(e) }
  fn main() -> i32 {
    e := mk(42i32)
    sink(mk(7i32))
    sink(mk(9i32))
    // get(e) yields 0, not 42.
    ret 0
  }
  ```

  It passes today whenever the dead frame still holds the payload by
  accident, which is why nothing catches it. The payload belongs inline
  in the enum's own slot, laid out from offsets the analyzer publishes
  for both the emitter and the lowerer; see `docs/adr/0014`.
  `Option<T>` is the enum every container accessor returns, so this is
  what blocks `Vec`.
- Reborrowing, and the coercions that go with it: `&mut T` used where a
  shorter `&mut T` or a `&T` is expected, including as a method
  receiver. Without it a method taking `&self` cannot be called through
  a `&mut` binding. A place reached through a reference has no
  representation in the borrow checker, whose places are a root
  register plus a field path, so a borrow of `*b` and a store to `*b`
  both resolve to `b` and a write through a dereference is not checked
  against a live loan. The rules are settled and the extent is
  non-lexical; see `docs/adr/0012`.
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
