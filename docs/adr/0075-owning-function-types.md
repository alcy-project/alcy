# ADR-0075: Owning function types

- Subject: the language
- Status: Accepted
- Date: 2026-10-16

## Context

A closure literal's nominal type is per-literal and unspellable, and
the bare function type `(T) -> R` is a `Copy` view that borrows the
environment. So an owning closure cannot cross a function boundary in
any spelling today: a `Func` return carries a borrow of the dead
frame and is refused, an omitted arrow means unit, and `Box`, tuple,
and struct members all need the nominal spelled or inferred. Heap
placement alone (ADR-0064's deferred slice) enables zero new programs
for this reason: the wall is naming, not allocation.

## Decision

**`fn(params) -> ret` in type position names an owning function
value.** `(T) -> R` keeps meaning a borrowing view; `fn(T) -> R`
means a value that owns its environment, drops it, and never copies:

- Each `fn()` occurrence is a fresh anonymous type parameter with an
  automatic `Fn` bound, monomorphized per witness. The compiler knows
  the implementation: binding a value to a `fn()` type checks the
  signatures match, and every use instantiates to the bound witness.
  No `Fn` spec machinery is needed for this: the bound is structural
  until specs can express it, and a real `Fn` spec joins later.
- A `fn()` value is move-only, unconditionally: even a `Copy`
  witness. Un-annotated `Copy` closures keep copying; annotating one
  with `fn()` moves it. One rule, no per-witness variance to track.
- In signatures (parameters, returns) the anonymous parameter belongs
  to the item and each caller instantiates it. In `let` annotations
  it belongs to the binding and the initializer fixes the single
  witness: reassigning a `fn()`-typed binding is refused, because two
  witnesses cannot share one abstract slot. `fn()` inside `Box`,
  tuples, and other spellings introduces the parameter at the same
  scope. Struct fields and `impl` blocks are out: naming storage
  needs named generics threaded by hand, which stays spelled.
- Coercions: a nominal coerces to `fn()` by signature match; a `fn()`
  coerces to a `( )->` view, which borrows the owned value and never
  outlives it. A view never coerces to `fn()`: it owns nothing.
- The witness's environment lives on the heap exactly when a move
  capture needs it owned (ADR-0064's placement rule, unchanged); the
  drop walk ends the fields and frees the block, statically, per
  instantiation. Returning a closure whose environment holds a borrow
  still reports: the loan still names the dead frame.

## Consequences

`fn make() -> fn(i32) -> i32` returns an owned closure; `f: fn(i32)
-> i32 = make()` holds one; adapters take them by value. What each
costs is explicit: one instantiation per literal, no heterogeneous
collections, no named storage. Summaries must carry closure bodies
for downstream monomorphization, which the summaries slice records.
Separate compilation stays whole-program-shaped, the way generics
already are. Out of scope, each its own decision: named storage of
abstract function values, reassignment, fat pointers for dynamic
dispatch, and a real `Fn` spec (which unlocks explicit bounds and
`Box<dyn>`-shaped designs, not this one).
