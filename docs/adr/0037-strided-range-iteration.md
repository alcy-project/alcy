# ADR-0037: Stride Lives on the Cursor

- Subject: the language
- Status: Accepted
- Date: 2026-10-02

## Context

ADR-0034 gave integer ranges a cursor that ascends one value at a
time, with the inclusive end reported exactly once and never stepped
past. Strided iteration is the remaining range API, and it meets that
rule head-on: with a stride, the step after the last value can
overshoot the end and — under the language's wrapping arithmetic —
wrap around into the admitted side. `254u8` with stride 2 would
otherwise continue at 0, which reads as in range. The design also has
to answer where the stride lives, what type it takes, and what a
non-positive stride means, all as ordinary library code with no
compiler support.

## Decision

**The stride is a cursor field, built by `Range::step_by`.**
`(0..<10).step_by(2)` names a `RangeIter` advancing two at a time,
and a `for` over it reads through an identity `into_iter` on the
cursor — no new type, no new spec implementation. A separate adapter
would leave the cursor frozen but double the surface for one
parameter; the cursor already owns iteration, so it owns the pace.

**The stride takes the element type.** `current + stride` and every
comparison stay inside `T` with no casts, and a `2` at the call site
adapts like any literal. A `usize` stride would read as the purer
count but needs a generic conversion the language has not proven,
plus truncation semantics at every narrowing width.

**A non-positive stride fails.** Zero has no meaning as a step, and
neither does a negative one for ranges that never yield downward;
`step_by` reports either before iterating rather than yielding one
value forever. One unsigned comparison covers both, since the `0`
adapts to the element type.

**The stop rule generalizes the end rule.** The cursor already asks
its end whether a value is admitted; it now also asks whether the
cursor is spent after yielding it. An inclusive end hit exactly ends
the run, and a stride that would pass the end stops first instead —
decided with differences that stay exact, because a yielded value
always lies on the admitted side. At stride one the rule coincides
with the old one, so every existing run reads unchanged.

## Consequences

What this buys is the last range API: stepped heads over every
integer width from one generic implementation, with the
width-dependent behavior — where the maximum stops — falling out of
the rule rather than the spelling. Inverted ranges stay empty at any
stride, open ends stride until the loop stops them, and the `u8`
maximum stops instead of wrapping.

What it costs is one field, one constructor, one identity method,
and one predicate replacing another. What is explicitly out of
scope: descending iteration, which stays empty rather than yielding
downward; re-striding a cursor, which is built with its pace;
and any further adapter, which a future proposal can carry on its
own.
