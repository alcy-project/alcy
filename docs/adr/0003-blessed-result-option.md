# ADR-0003: Deferred Generics with Blessed Result and Option

- Status: Accepted
- Date: 2026-09-18

## Context

The error model needs `Result<T, E>` and `Option<T>`, which are
generic, while user-defined generics (plus `spec` dispatch and
monomorphization strategy) are too large for MVP. Deferring all
generics would leave MVP programs without error handling; allowing
general generics would blow up MVP scope.

## Decision

- `Result<T, E>` and `Option<T>` are compiler-known, monomorphized
  blessed types: the only generics in MVP.
- User-defined generic types and functions are deferred and are a
  prerequisite for the bootstrap (not for MVP).
- MVP `?` propagates identical error types only; `From`-style
  conversion arrives with the spec system.

## Consequences

- MVP gains usable error handling at the cost of one blessed
  monomorphization path in the backend.
- No user code may define generic items; diagnostics must say so
  explicitly rather than failing obscurely.
