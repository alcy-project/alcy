# Errors, Panic, and Divergence

## Error types in the standard library

- `Result<T, E>` and `Option<T>` are ordinary generic enums defined in
  the `alcy/std/core` prelude. The compiler holds no knowledge of
  their names, shapes, or methods; a user module may shadow either
  name. See `docs/adr/0009-result-option-library-enums.md`.
- `Result` has variants `Ok(T)` and `Err(E)`; `Option` has `Some(T)`
  and `None`. Both compose as ordinary algebraic types for region
  purposes (field intersection and projection apply unchanged).
- Core provides the `or` family (`or`, `or_else`, `or_option`,
  `or_panic_with_message`, `or_panic_with`), the predicates (`is_some`,
  `is_none`, `is_ok`, `is_err`), and the closure combinators (`map`,
  `and_then`, `filter`, `ok_or`) as ordinary `impl` methods. No method
  named `unwrap` or `expect` exists, and none may be added (ADR-0088):
  a name that can abort says so.

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
  innermost first, before aborting (see `docs/adr/0070-a-panic-can-carry-a-backtrace.md`).
  A frame with a known call site appends it: `at f (main.al:6:3)`.
  An optimized build shows the frames that survived inlining, which can
  be just `main`; a target whose frame pointer is not that chain -
  Windows x64's is an RSP-relative point in the fixed frame by design -
  and the wasm backend everywhere, write the message and its site only.
- The `!` (never) type exists: `panic` has type `!`, and `!`
  coerces to any type. Looping constructs that cannot fall through
  also produce `!`.

## Entry point

- `fn main()` returns `()`, `i32`, or a two-variant enum whose first
  variant carries a single `()`. `Result<(), E>` from core is the
  intended form.
- A nonzero discriminant from the entry enum aborts with a fixed
  message and a nonzero exit status; the error payload is never
  printed (no `Debug` bound exists).

## Tests

- A `test` block states a module's tests. It sits at module level, does
  not nest, and its entries and helpers are never `pub`; each of those is
  an error where it is written.
- An entry takes no arguments, and the empty parameter list is syntax.
  Success is returning, so an entry's return type is `()`. A written
  return type is reported by the command that runs tests rather than by
  the parse, which is what keeps a build from failing over a body it
  never emits.
- A block item's name must be free in the module the block joins. A name
  the module already declares, a name a second block item takes, and
  `main` are each refused: the first two put one name in one namespace
  twice, and the third takes the entry point a test run supplies itself.
  See `docs/adr/0139-a-test-is-an-item-the-test-command-owns.md`.
- A test fails by panicking, and a panic is all a test can fail with:
  there is no error value and no unwinding, so an expectation is an
  ordinary expression that panics when it does not hold, and the
  panic's message and source position are what says why the test failed.
- `alcy test` tests one package. A suite names the packages that carry
  the tests rather than carrying them itself, so it is refused with the
  member's directory to give instead, and a target that starts no
  processes - a freestanding profile, or the wasm backend - is refused
  because a test is a process the run starts and watches.

## Unused values

- An expression statement whose value is neither `()` nor `!` warns.
  Write `_ := expr` to discard deliberately. The rule is uniform
  across all value types and does not special-case any type.
