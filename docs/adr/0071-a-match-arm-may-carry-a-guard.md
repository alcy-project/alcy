# ADR-0071: A match arm may carry a guard

- Subject: the language
- Status: Accepted
- Date: 2026-10-14

## Context

`match` chooses an arm by shape alone. A decision that also needs a
condition - `Some(v) if v > 0` - has to nest a match or an `if`
inside the arm body, repeating the shape test and pushing the
condition one level down. Guards were deferred with the pattern
work; the question is what a guard may read and how it bears on
exhaustiveness.

## Decision

**An arm is `pattern if cond => body`, and the guard is checked
with the arm's bindings in scope and must be a bool.** A false
guard falls through to the next arm, so the guard filters the arm
rather than naming a separate one. It follows the pattern, reads
what the pattern bound, and cannot bind names of its own.

**A guarded arm does not count toward exhaustiveness.** The guard
may be false for every value the pattern admits, so coverage
requires an unguarded arm for the same case; the checker reports a
missing case exactly when the unguarded arms leave one.

## Consequences

The shape test is written once, and a condition on a payload sits on
the arm line. The conservative coverage rule can ask for an arm that
a guard makes unreachable at run time, which is the bargain the
wildcard arm already strikes. Lowering is one block per guarded arm:
the pattern test branches to the guard, and the guard branches to
the body or to the next arm. String-literal patterns and slice
patterns remain deferred.
