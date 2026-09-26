# debug

Internal-invariant toolkit: `DCHECK`, `DLOG`, `UNREACHABLE`, and
related helpers.

The project builds with `-fno-exceptions` and `-fno-rtti`: no
`try`/`catch`/`throw`, no `dynamic_cast`. Internal compiler
invariant failures use `DCHECK()` (or `UNREACHABLE()` for provably
dead branches over verified input); anything reachable by user
input uses explicit error reporting (`base::Result`, diagnostics)
instead.

## Input requirements

- `DCHECK`/`UNREACHABLE` must never guard user input: if a test or
  a user can construct the value, it needs a verifier and a
  `Result`, not an assertion.
- `UNREACHABLE` documents *why* the branch is dead (which proof
  rules it out); a bare `UNREACHABLE()` on unvalidated data is a
  bug.
