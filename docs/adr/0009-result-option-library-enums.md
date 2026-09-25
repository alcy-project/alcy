# ADR-0009: Result and Option as Library Enums

- Status: Accepted
- Date: 2026-09-25
- Supersedes: ADR-0003

## Context

ADR-0003 introduced `Result<T, E>` and `Option<T>` as compiler-synthesized
"blessed" types because MVP had no user-defined generics. That decision
is now obsolete: generic enums, generic inherent methods with per-value
monomorphization, and the `Enum::Variant` path all exist. The blessed
machinery has become a second, parallel type system that every stage
must special-case.

The blessed approach carries costs that no longer buy anything:

- A reserved-name check forbids users from defining `Result` or
  `Option`, so the obvious spellings are unavailable.
- Constructors, patterns, methods, `?`, must-use, and the entry-point
  return check each have a blessed branch alongside the generic branch.
- The blessed registry is a second identity space parallel to the IR
  type table, so lowering must translate between two numbering schemes.
- `unwrap`/`expect`/`is_ok`/`is_err` are compiler-known method names
  rather than ordinary methods with ordinary signatures.

## Decision

`Option<T>` and `Result<T, E>` are ordinary generic enums defined in
`lib/std/core/main.al`. The compiler keeps no knowledge of their names,
their shapes, or their methods. Everything that used a blessed branch
now uses the generic path that already exists for user enums.

Three behaviors survive the transition because they are part of the
language rather than of the types, and each is now expressed as a
structural rule over any enum:

- `?` propagates only within an identical enum type. The scrutinee and
  the enclosing return type must be the same enum instantiation. On the
  first variant the operator yields that variant's first payload; on
  any other variant it returns the scrutinee unchanged. This preserves
  ADR-0003's "identical error types only" rule without naming a type.
- `main` may return `()`, `i32`, or a two-variant enum whose first
  variant carries a single `()` payload. The entry thunk maps the first
  variant to exit code 0 and aborts on any other discriminant.
- The unused-value warning applies uniformly to every non-`()`,
  non-`never` expression statement, so `Result`/`Option` need no
  dedicated must-use concept.

The LLVM entry thunk already decoded the enum slot structurally, so
code generation keeps its existing `TypeTag::Enum` behavior.

## Consequences

- The blessed registry, `intern_blessed`, `blessed_find`,
  `resolve_blessed_ctor`, `PathValue::BlessedCtor`, the
  `CompValue::Blessed` tag, `lower_blessed_method`,
  `load_blessed_payload`, and `mark_blessed_covered` are deleted.
- `unwrap`, `expect`, `is_ok`, `is_err`, `or`, and friends are ordinary
  `impl` methods. Users may define types with those methods, and may
  define their own `Result` or `Option` in a module that shadows the
  prelude.
- `?` is no longer specific to error types. Any two-variant enum can
  use it, which makes the operator a general early-return mechanism.
  Type conversion between different error types still requires an
  explicit `match`, and remains deferred with the spec system.
- The `CheckedPackage` exposes one instantiation table instead of two,
  so lowering table keys become uniform.
- The parser, lexer, AST, and IR are unchanged: `?`, generic type
  arguments, and enum payloads were already represented generically.
