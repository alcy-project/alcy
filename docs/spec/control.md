# Control Flow (MVP)

## Match

- `match` requires exhaustive arms over integer/bool literals, unit
  and tuple variant patterns, tuple and struct patterns, and `_`.
  Bindings move by default; `&`/`&mut` patterns borrow.
- Deferred: guards, string literal patterns.

## Blocks, values, and sequencing

- A block's value is its last expression (positional rule). Statements
  sequence by newline; `;` separates multiple statements on one line
  only.
- Expression statements silently discard `()` values. Discarding a
  non-`()` value SHOULD be explicit (`_ := ...`); `Result` and
  `Option` values are hardcoded `must_use` and produce a warning
  diagnostic when discarded.
- A non-`()` trailing expression where `()` is expected is a type
  error suggesting explicit discard.
- `if` without `else` is statement-only; in value position a missing
  `else` behaves as `else { () }` with unification. Loops evaluate
  to `()` (`break` with a value is deferred).
- `ret expr` returns early from the enclosing function. `if a := b`
  binds in the then-branch only (see `items.md` for pattern sharing).
