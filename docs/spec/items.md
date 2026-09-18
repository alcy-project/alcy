# Items (MVP)

## Bindings

- Declarations use `:=`, which always introduces bindings:
  `x := expr`, `mut x := expr`, `x: T := expr`, `mut x: T := expr`.
- Reassignment uses `=` which MUST refer to an existing `mut` binding.
- A `:=` in one scope MUST introduce at least one new binding;
  otherwise it is a compile-time error suggesting `=`.
- Declaration left-hand sides share the pattern grammar with `match`
  (see `control.md`): `(c, _) := ...` destructures, `_ := ...`
  explicitly discards a value (silencing `must_use`; see `control.md`).

## Functions and associated items

- `fn` declares free functions. Signatures carry explicit types;
  bodies infer locals intraprocedurally (integer literals default to
  `i32`, float literals to `f64`).
- Inherent `impl` blocks are MVP; they require no generics machinery.
- The compiler provides a `print(msg: str)` intrinsic, lowered
  directly to a write syscall. It migrates to an ordinary core
  function once FFI lands.
- `static` items have storage and MUST NOT contain `&mut`.
  `const X: T = ...` items are inline constants restricted to literal
  expressions in MVP (full const evaluation arrives with `comp fn`,
  post-MVP). Binding-position `const` does not exist.

## Structs and enums

- Structs have named fields only, no constructors, and optional
  user-defined destructors. Construction initializes every field;
  `..base` move-update is allowed.
- Enums have unit and tuple variants only (`Ok(T)`/`Err(E)` and
  `Some(T)`/`None` are the canonical examples). Struct variants,
  explicit discriminants, and layout guarantees are deferred; default
  layout is compiler-chosen.
