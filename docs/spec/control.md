# Control Flow (MVP)

## For

- `for pat in head block` iterates a cursor obtained from the head:
  `head.into_iter()` runs once, and each iteration binds `pat` to the
  next item until the cursor reports the end. `into_iter` is an
  ordinary method resolved in the caller's scope; a head whose type
  does not have one is an error naming that method.
- The cursor must implement core's `Iterator` for the item type. The
  loop reaches `next` through that spec and never through an inherent
  method of the same name, so an `Iterator` implementation must be in
  scope or the loop is an error.
- `Some(item)` binds `pat` against the item, and `None` ends the
  loop. The pattern must match every item: a refutable pattern is an
  error naming the loop, never a filter. Write the `loop` out when
  some items should skip the body.
- A range head names its end explicitly: `for i in 0..<n` yields
  `0` through `n - 1` and `for i in 0..=n` yields `0` through `n`
  (see `types.md`). A stepped head spells the stride on the range:
  `for i in (0..<n).step_by(2)`.
- `for` is a block-like expression evaluating to `()`, so it works as
  a statement and in value position. `break` and `continue` in the
  body act on this loop, and nested loops keep independent cursors.

## Match

- `match` requires exhaustive arms over integer/bool literals, unit
  and tuple variant patterns, tuple and struct patterns, and `_`.
  Bindings move by default; `&`/`&mut` patterns borrow. A binding
  behind a reference is a shared reference: `match *e` with `e: &E`
  binds `s: &T`, the referent stays its owner to end, and moving
  through `*s` is refused.
- A negative integer or float literal is a pattern (`-1 => ...`), and
  compares by value after the sign is applied, so `-1` does not match
  `1`.
- An arm may carry a guard: `pattern if cond => body`. The guard is
  checked with the arm's bindings in scope and must be a bool. A
  false guard falls through to the next arm, and a guarded arm does
  not count toward exhaustiveness, so an unconditional arm is still
  required.
- A tuple, struct, or variant payload pattern may itself be
  refutable: a literal, a unit variant, or a variant whose payloads
  are tested in turn. An or-pattern nested anywhere distributes
  into one arm per alternative before lowering.
- Deferred: string literal patterns.

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
- `ret expr` returns from the enclosing function. A non-`()` function
  body must end every path in `ret` or an expression of type `!` (a
  `panic`, an `if`/`match` whose branches all diverge), so the body's
  tail value is never its return value; a trailing `;` after `ret`
  leaves the statement diverging all the same. Unit functions need no
  `ret`. `if a := b` binds in the then-branch only (see `items.md` for
  pattern sharing).
