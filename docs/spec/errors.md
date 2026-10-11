# Errors, Panic, and Divergence (MVP)

## Error types in the standard library

- `Result<T, E>` and `Option<T>` are ordinary generic enums defined in
  the `alcy/std/core` prelude. The compiler holds no knowledge of
  their names, shapes, or methods; a user module may shadow either
  name. See `docs/adr/0009-result-option-library-enums.md`.
- `Result` has variants `Ok(T)` and `Err(E)`; `Option` has `Some(T)`
  and `None`. Both compose as ordinary algebraic types for region
  purposes (field intersection and projection apply unchanged).
- Core provides `unwrap`, `expect`, `is_ok`, `is_err`, and `or` as
  ordinary `impl` methods. Combinators requiring closures (`map`,
  `and_then`) arrive with closures, post-MVP.

## The `?` operator

- `?` is structural, not type-specific. It requires an enum operand
  and an enclosing function whose return type is the *same* enum.
- On the first declared variant the operator yields that variant's
  first payload. On any other variant it returns the operand
  unchanged, which makes the early return the propagation path.
- Only identical types propagate. Converting between different error
  types requires an explicit `match`; automatic conversion
  (`From`-style) requires the spec system and is deferred.

## Panic and the never type

- `panic(msg: str)` diverges and aborts the process. There is no
  unwinding, and drops do not run on the panic path.
- A panic writes `panic at <file>:<line>:<column>: <message>` to
  standard error, so the abort says where it happened. A panic with no
  source position - the entry thunk's fixed message, an allocation
  failure - writes `panic: <message>`. The runtime's text is fixed and
  not localized; only compiler diagnostics follow `--lang`.
- A profile with `on-panic = "backtrace"` additionally prints one
  indented `at <name>` line per program frame the panic walked through,
  innermost first, before aborting (see `docs/adr/0063-a-panic-can-carry-a-backtrace.md`).
  A frame with a known call site appends it: `at f (main.al:6:3)`.
  An optimized build shows the frames that survived inlining, which can
  be just `main`; a target other than x86-64 or AArch64, and the wasm
  backend everywhere, write the message and its site only.
- The `!` (never) type exists from MVP: `panic` has type `!`, and `!`
  coerces to any type. Looping constructs that cannot fall through
  also produce `!`.

## Entry point

- `fn main()` returns `()`, `i32`, or a two-variant enum whose first
  variant carries a single `()`. `Result<(), E>` from core is the
  intended form.
- A nonzero discriminant from the entry enum aborts with a fixed
  message and a nonzero exit status; the error payload is never
  printed (no `Debug` bound exists in MVP).

## Unused values

- An expression statement whose value is neither `()` nor `!` warns.
  Write `_ := expr` to discard deliberately. The rule is uniform
  across all value types and does not special-case any type.
