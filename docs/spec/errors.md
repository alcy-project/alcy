# Errors, Panic, and Divergence (MVP)

## Blessed generics

- `Result<T, E>` and `Option<T>` are the only generic types in MVP.
  They are compiler-known and monomorphized. User-defined generics
  are deferred (see `deferred.md`).
- `Result` has variants `Ok(T)` and `Err(E)`; `Option` has `Some(T)`
  and `None`. Both compose as ordinary algebraic types for region
  purposes (field intersection and projection apply unchanged).
- The compiler provides exactly `unwrap`, `expect`, `is_ok`, and
  `is_err`. Combinators requiring closures (`map`, `and_then`) arrive
  with closures, post-MVP.

## The `?` operator

- `?` propagates the error (or `None`) to the enclosing function,
  which MUST return a matching `Result`/`Option` type.
- Only identical error types propagate in MVP. Automatic conversion
  (`From`-style) requires the spec system and is deferred; mismatched
  error types are compile-time errors resolved by explicit mapping.

## Panic and the never type

- `panic(msg: str)` diverges and aborts the process. There is no
  unwinding, and drops do not run on the panic path.
- The `!` (never) type exists from MVP: `panic` has type `!`, and `!`
  coerces to any type. Looping constructs that cannot fall through
  also produce `!`.

## Entry point

- `fn main()` returns `()`, `i32`, or `Result<(), E>`. An `Err` from
  `main` aborts with a fixed message and a nonzero exit status; `E`
  is never printed (no `Debug` bound exists in MVP).
