# ADR-0004: Whole-Program Analysis with Per-Package Emission

- Status: Accepted
- Date: 2026-09-18

## Context

Whole-program compilation (all dependencies visible to analysis and
optimization) conflicts with modularity (separate artifacts,
encapsulation, incremental rebuilds) if emission is also whole-program.
The design must keep both: maximal analysis visibility and
package-shaped outputs.

## Decision

- Closed-world analysis, open-world artifacts: analysis and
  optimization may see every dependency's IR and summaries, while
  emitted artifacts stay per package (`rlib`-equivalent carrying
  summaries, designed later).
- `pub` API is conservatively preserved across package boundaries;
  true whole-program optimizations over public items are explicitly
  out of scope.
- Cyclic package dependencies are forbidden (already enforced by
  dependency resolution).

## Consequences

- Package archives MUST carry function summaries; the archive format
  is a future design with this ADR as its constraint.
- MVP compiles single-package programs only; no cross-package
  machinery exists yet.
