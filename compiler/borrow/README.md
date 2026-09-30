# borrow

Ownership checking over lowered IR: use-after-move, borrow
exclusivity, assignment to borrowed places, and reference escape
from returned values.

Moves flow forward through the CFG with per-block join states and a
loop fixed-point; loans expire at last use. Interprocedural
precision comes from function summaries reified at call sites.

## Entry points

- `check_borrows(lowered, bag)` ->
  `base::Result<void, diag::Reported>`. Findings accumulate in the
  bag; err marks a package that gained errors during the call
  (pre-existing bag errors are not attributed to it).

## Input requirements

- The package's storage arrives as `ir::VerifiedStorage` proof from
  lowering, so no entry re-verification runs. Never hand-feed
  unverified storage: there is no checked constructor for it.
