# ADR-0059: A function returns with `ret`

- Subject: the language
- Status: Accepted
- Date: 2026-10-09

## Context

A block's value is its last expression, and a function body is a
block. Previously `check_fn` accepted that tail expression as the
return, so `fn answer() -> i32 { 42 }` and
`fn f() -> i32 { if c { 1 } else { 2 } }` type-checked; `ret` was
one way to return, not the way. The cost there outweighed that
flexibility.

That flexibility has costs the language has already voted against at
the call site, where `ret` is the convention nearly everywhere in
`lib/std`, the cases, and the docs:

- **Explicitness.** Nothing marks the value that leaves a function. A
  reader looking for what a function returns has to know the positional
  rule and find the last expression of every path.
- **Grepability.** `ret` is the one token every return shares. With
  implicit returns, `grep ret` answers "where does this code return?"
  with an incomplete list, and no other token replaces it.
- **Bug prevention.** A value left at the end of a body silently
  becomes the return, so an edit that removes a `ret` can keep
  compiling with a different meaning; and a stray expression appended
  to a body becomes its return. Both are exactly the class of change a
  mandatory `ret` makes loud.

The one-line exception that was considered, "a body that fits on one
line may keep its tail value", was rejected: semantics that depend on
layout change under a formatter or a reflow, which no tool can be
allowed to do to meaning. If terseness for small functions is wanted
later, it belongs in an explicit expression-body form (`fn f(x: T) ->
U = expr`), which is its own decision.

## Decision

A function with a non-`()` return type must **diverge**: every path of
its body ends in `ret` or in an expression of type `!` (`panic(...)`,
an `if` or `match` whose branches all diverge, another `ret`-ending
block). The body's tail value is not the return value.

```alcy
fn answer() -> i32 {
  ret 42
}

fn sign(x: i32) -> i32 {
  if x < 0 {
    ret -1
  } else {
    ret 1
  }
}

fn unreachable_answer() -> i32 {
  panic("no answer")
}
```

- A trailing `;` after `ret` is the same divergence: a block whose
  last statement has type `!` does not fall off the end, so
  `{ ret x; }` is a valid body.
- `()` functions are unchanged: they need no `ret`, and a trailing
  non-`()` value is the error it already was.
- Closures keep their own form. A closure body is one expression or a
  block whose value is its last expression (`grammar.md`):
  `(x: i32) -> x + 1` and `(x: i32) -> { x + 1 }` stay as they are.
  The rule names functions; the closure's arrow is its own spelling.
- A function declaring `-> !` still must diverge, which is the same
  rule with a different return type. `main`'s special cases, `comp`
  functions (which already require a tail `ret`), and early `ret`
  through branches are unchanged.

The rule is checked by the analyzer, not the parser: `ret` was already
optional in the grammar, so no syntax, tree-sitter grammar, or
lowering changes. A body's type comes from its last statement as well
as its value expression, which is what lets a trailing `ret;` count as
divergence.

## Consequences

- Three in-tree sources migrated (`ok-negative-pattern`,
  `exe-negative-pattern`, `exe-range`: match arms that returned values
  now say `ret`), and one lowering test's mixed arm gained its `ret`.
  Nothing else in `lib/std`, the cases, or the docs relies on the
  implicit form, which is the evidence that the convention was already
  the rule and only the analyzer lagged.
- A new diagnostic names the return type: "A function returning '{}'
  must end every path with 'ret'". It replaces the type-mismatch
  message an empty or value-ended body used to get, and points at the
  tail expression when there is one.
- Blocks keep their positional value everywhere else: `x := if c { 1 }
  else { 2 }`, match arms, `unsafe { ... }`, and `comp` blocks are
  untouched.
- A function body that ends in a diverging *statement* (`ret x;`) is
  accepted through the same "last statement's type" reading, so the
  semicolon habit costs nothing.

## Alternatives considered

- **Keep tail-value returns.** Rejected: it is the status quo the
  language's own style already avoids, and it leaves returns
  ungrepable.
- **A layout exception** (a one-line body keeps its tail value).
  Rejected: meaning would depend on line breaks.
- **An AST-shape exception** (a body that is exactly one expression
  keeps it). Rejected for the same reason as any exception here: the
  rule would be "always, except some bodies", and `ret x` is shorter
  than the explanation. If short bodies matter, an explicit
  `= expr` form is the honest spelling.
- **Enforce it in a linter rather than the analyzer.** Rejected: a
  lint cannot stop the code from compiling with two readings; this is
  a semantic rule, not a style preference.
