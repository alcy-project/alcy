# ADR-0135: The extent rule follows the loan

- Subject: the language
- Status: Accepted
- Date: 2026-11-01

## Context

A reference stored into a binding that outlives its referent reads a
slot whose block has ended, which is a use-after-free the language
refuses rather than leaves to the sanitizers. Blocks are a stack, so a
local declared after the destination's lives in the destination's block
or deeper and its slot cannot outlast the store: the destination's
position among the function's locals is the extent, and comparing two
positions answers the question without a region computation.

Reading the rule off the stored expression alone is what left it open.
`r = &m` is caught because the value is a `&`, but `r = id(&m)`,
`r = p` for an alias `p := &m`, and `r = holder.p` hand the same
reference over through a call, a copy, or an aggregate, and none of
those spell a `&` at the store. The program then reads a dead slot and
the compiler accepted it.

## Decision

**The extent comparison runs on what the stored value borrows, not on
the syntax that spelled it.**

- `&place` records the local `place` is rooted at, and a read of a
  reference-typed local records what that local was recorded to name, so
  a loan survives a call, a copy, and an aggregate in between: a
  binding's `referents` are copied from the borrows its own lowering
  made, an alias copies its source's `referents`, and a store into a
  path hands its sources on to the destination.
- A store is refused when a value whose type carries a borrow - itself,
  or in a field, an element, or a variant payload - names a source
  declared after the destination.
- A reference from a parameter, or from a call that kept its input to
  itself, names no place in the body and passes.
- A call that writes through a place can keep what it is handed:
  `Vec::push` writes its element into the buffer through an unsafe
  intrinsic inside the library, so no place in the caller holds the
  reference and the store never meets the rule. A value given for a
  parameter that is neither a view nor a reference is compared with
  every place the call can write through, as if the store happened at
  the call, and a temporary's slot is a source like a named local is. A
  shared loan cannot store, and a parameter that is itself a view or a
  reference borrows for the call and keeps nothing, so a value handed
  over that way passes.

The check stays in lowering rather than in the borrow pass because the
borrow pass reads IR, which keeps no scope extents: the passes' blocks
are not the source's blocks, and `{ n := 5; }` and the same binding
without braces give the same blocks and the same loan liveness, so an
extent compared there would answer a different question.

## Consequences

- `r = id(&m)`, `r = p`, and `r = holder.p` are refused with the
  same diagnostic as `r = &m` (`err-borrow-call-escape`,
  `err-borrow-alias-escape`, `err-borrow-call-alias-escape`), and the
  legitimate shapes stay accepted (`ok-borrow-call-inner`,
  `ok-borrow-alias-param`, `err-borrow-outlives-scope`).
- The rule is conservative where the derivation is: a value that reads a
  later reference without keeping it (`f(&&a, s)` returning the first
  argument's referent) is refused even though the store is safe. The
  alternative is the outlives relation between regions that
  `docs/spec/deferred.md` leaves to the region-annotated captures work
  (ADR-0076), which this rule stands in for.
- A view kept by a container is refused (`err-kept-reference-local`,
  `err-kept-reference-temporary`) while the same view of a buffer the
  container is declared after is accepted (`ok-view-outlives-container`),
  which is the order the drop walk ends them in.
- The rule is an extent rule, not a lifetime analysis: loans are still
  liveness-checked by the borrow pass, and a reference that never meets a
  store of a shallower binding is unaffected.
