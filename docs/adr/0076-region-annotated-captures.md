# ADR-0076: Region-annotated captures with inference

- Subject: the language
- Status: Accepted
- Date: 2026-10-16

## Context

Loans carry their parents, so a reborrow reads through to the loan it
derives from; what the region model still lacks is the relational
part, recorded in `docs/spec/deferred.md`: the outlives constraints
between regions and the intersection a struct composes by. The gap
shows at function boundaries. A closure that captures a borrow of one
of its own locals cannot escape — the frame dies, and refusing that
is obviously right. But a closure that captures a borrow handed in
from outside dies with the frame only by conservatism: the borrow
belongs to the caller, and the closure could live exactly as long.

## Decision

**Captures carry regions, inferred, never written.** Every borrow
capture, every reference-typed binding, and every frame gets a region
variable; uses add outlives constraints; a value crosses a boundary
only where every region it names outlives the destination. A local
borrow's region ends at its frame, so frame escape stays refused by
the same rule that lets a caller-region borrow through. There is no
lifetime syntax and none is planned: inference is total, and
diagnostics name spans rather than regions, the way every other
borrow diagnostic already does.

## Consequences

`fn make(t: &str) -> fn() -> usize` compiles when the returned
closure captures only `t`: the capture's region is the caller's, which
outlives the return. `ret [local] ...` still reports, with the local's
region named by its span. This completes the relational remainder the
deferred list records; that entry now points here. What the solver
must still settle in implementation: `&mut` captures (exclusivity
across calls, not just liveness), closures capturing closures
(regions compose through the witness), the split of work with the
loan-carrying the reborrow paths already do, and inference cost
(termination on cyclic constraints, and behavior before the fixpoint
in pathological programs).
