# ADR-0036: A Refutable `for` Pattern Is an Error

- Subject: the language
- Status: Accepted
- Date: 2026-10-02

## Context

`for pat in head` desugars to a `match` on the cursor's `next`,
so a refutable pattern *works* as a filter by construction: arms
that do not match simply do not run. The question is whether the
language should bless that reading. Two positions already answer
its equivalent: `:=` declarations and function parameters require
irrefutable patterns with dedicated check-time diagnostics, and the
specification states the `for` pattern must match every item —
without the implementation enforcing it. Until now a refutable
pattern failed late, with lowering's slice-limitation wording,
which states an implementation gap rather than a language rule.

## Decision

**A refutable `for` pattern is a check-time error.** The desugar
marks its generated `Option::Some(pat)` arm, and the match check
reports a refutable marked pattern with a dedicated message in the
existing family ("Refutable pattern in `for`; use `loop`"), reusing
the refutable-binding diagnostic code. Filtering stays expressible
by writing the `loop` out, which is what the message points at.

**Nothing else moves.** Hand-written matches keep lowering's
verdict on nested patterns, and `if x := ...` conditions stay
refutable by design — the marker is opt-in on the generated arm
only. No program that builds today changes meaning: every program
with a refutable `for` pattern already failed, only later and more
confusingly.

## Consequences

What this buys is the documented rule made true: declarations,
parameters, and loop patterns all require irrefutable bindings,
each diagnosed where it is written. The lowering slice errors
remain exactly where they were, as backstops for hand-written
nested patterns rather than as the voice of the `for` rule.

What it costs is one flag on a pattern node and one branch in the
match check. What is explicitly out of scope: nested refutable
patterns in hand-written matches, which stay with the pattern work;
and any filtering sugar, which a future proposal can carry on its
own.
