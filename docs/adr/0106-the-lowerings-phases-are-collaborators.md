# ADR-0106: The lowering's phases are collaborators

- Subject: the compiler
- Status: Accepted
- Date: 2026-10-29

## Context

`Lowerer` is one class spread over four files — `lowerer.h` (816
lines of declaration), `lowering.cc`, `lowering_expr.cc` (5432 lines),
and `lowering_comp.cc` (1823) — and it does three jobs: AST-to-IR
lowering, evaluation of what the checker marked `comp`, and the format
and `io` expansion. The backlog names it: "Split `Lowerer`: comp
evaluation, format expansion, and AST-to-IR lowering share one class
with a large public surface."

The jobs share one state: the builder, the module and instantiation
being lowered, the function tables, the symbol names, the comp
evaluator with its persistent bindings, and the diagnostics. A split
into independent classes would have to hand that state around, and the
seam is bidirectional — a format argument lowers as an ordinary
expression, and the expression lowering asks the comp evaluator
whether a value is known — so an "interface" would be most of the
class.

## Decision

**The phases become collaborators inside the class: nested types the
lowerer owns, each with a small surface the rest of the lowering
names.**

- A collaborator holds `Lowerer& owner_` and is defined *inside* the
  class, so it reads the owner's state without friend declarations and
  without becoming a second owner. The owner keeps the state.
- The first is `Lowerer::Comp` (`src/lowering/comp_lowering.cc`): the
  four methods the rest of the lowering names (`evaluate`,
  `evaluate_item`, `bind_pattern`, `materialize`), while its failure
  report stays its own; the owner holds one as `comp_lowering_`.
- The second is `Lowerer::Fmt` (`src/lowering/fmt_lowering.cc`): the
  format and `io` expansion, entered through `lower_write`,
  `lower_format`, and `lower_print`, with its own private `State` for
  the measure-then-write pass. What remains — expressions, statements,
  places, the function tables — is the owner's, because that is what
  the other two call into.
- A collaborator may call another's public methods (the format
  expansion materializes comp values), so each names the surface it
  offers rather than hiding it behind the owner.

## Consequences

- The owner's declaration surface shrinks with each collaborator, and
  a phase's code and its entry points live in one file with one name.
- The state keeps one home, so no lifetime or aliasing question is
  introduced by the split; a collaborator is a view, not an owner.
- A collaborator is reachable in tests through the owner it was built
  for, and its surface is small enough to pin directly.
- The split lands in slices: comp evaluation first (ADR-0106's own
  patch), then the format expansion; each slice is a mechanical move
  plus the call sites' new name.

## Alternatives considered

- **Independent classes with a services interface** (a struct of
  function references or callbacks). Rejected: the seam is
  bidirectional, so the interface would carry most of the class's
  surface, and every phase would still be written against the owner's
  state.
- **Free functions taking `Lowerer&`.** Rejected: the access problem
  returns as friendship spread over many symbols, and a call site
  loses what a member reads with.
- **Moving the comp state (the evaluator, its bindings) into the
  collaborator.** Rejected: the expression lowering reads the same
  state (comp arguments, comp-known bindings), so it belongs to the
  owner; a collaborator that owned it would need the owner to ask it
  back for every read.
