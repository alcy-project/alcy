# ADR-0093: `?` propagates without converting

- Subject: the language
- Status: Accepted
- Date: 2026-10-27

## Context

Rust's `?` calls `From::from` on the error it propagates, so a
function's error type is assembled from whatever conversions are in
scope. The call site does not name the conversion, and adding one
`impl From<A> for B` in a dependency makes `?` sites compile that did
not, or changes the error a caller sees. The error type becomes the
join of its consumers rather than a declared shape.

`docs/spec/errors.md` states alcy's rule: `?` requires an enum operand
and an enclosing function returning the same enum, the first variant's
first payload is the value, and anything else returns the operand
unchanged. That reads as a staging note ("conversion is deferred"), which
invites a later slice to add the conversion. The rule is the decision.

## Decision

**`?` is structural and never converts. A propagation between
different error types is an explicit `match`, and the standard
library's errors are ordinary values with the narrowest type that
answers the caller.**

- The operand and the enclosing function's return type are the same
  enum; `?` yields the first declared variant's first payload and
  early-returns every other variant unchanged. Nothing inspects the
  payload's type.
- No `From`-shaped spec exists or may be added, and no `?` calls a
  conversion. Where two error types meet, the author writes the
  translation — one arm, at the site that knows both.
- No `Try` spec: the operator is a language form over enums, not a
  desugaring into a user-implementable protocol. A second
  implementable path would hide the control flow the reader is
  checking for.
- The standard library states a failure as the narrowest value that
  answers the question: absence is `Option`, a success flag is `bool`
  (`io::read_file`, `io::write_file`). An error type arrives when a
  caller must distinguish causes, and it is an ordinary enum whose
  variants are exactly those causes. No error type carries a code plus
  a message bag.
- Nothing prints an error payload: there is no `Debug` bound, and the
  entry point's nonzero variant aborts with a fixed message
  (`docs/spec/errors.md`). A program that wants to report an error
  writes the message it means, through `or_panic_with`-shaped methods
  (ADR-0088) or its own formatting.

## Consequences

- A `?` site is readable alone: the operand's type and the function's
  return type decide everything, and no conversion table is consulted.
- Layered code pays for the translation it wants: a library whose
  error is a lower layer's reuses that type or writes one arm, rather
  than accumulating `From` implementations into one catch-all error.
- A single crate-wide error enum stays possible, and it is built by
  hand at the boundaries. That cost is the point: the boundary is
  where the mapping is a design decision.
- `Option` and `Result` propagate through `?` like any other
  two-variant enum, because the rule is structural. The declaration
  order is part of the contract: the first variant carries the value
  and the rest are the early return, which is why `Result<T, E>`
  declares `Ok` first. A two-variant enum written in the other order
  propagates its other variant, and that is visible where it is
  declared.

## Alternatives considered

- **`From`-based conversion, as Rust has.** Rejected: the conversion
  set is invisible at the call site, a dependency's implementation
  changes which call sites compile, and the error type becomes the
  union of its consumers.
- **A `Try` spec that `?` desugars into.** Rejected: the operator's
  meaning would live in implementations, and the early return — the
  thing a reader checks — would be spelled nowhere.
- **Printing the error's `Debug` at the entry point, as Rust's
  `Termination` does.** Rejected: a `Debug` bound on every error type
  for one convenience, and the printed shape is not a decision the
  author made.
