# Ownership and Regions (MVP)

## Borrow rules

- `&T` (shared reference): freely duplicable; any number may coexist.
- `&mut T` (exclusive reference): at most one live at a time, and no
  `&T` to the same place may be live while it is.
- A borrow of a temporary names a slot in the frame: `&2`,
  `&(1 + 1)`, and `&format("y")` all read a place. A `Copy` one has
  nothing to end; an owning one becomes an unnamed local the drop walk
  ends at the enclosing block, so the borrow is read and retired once
  and no reference outlives it.
- A non-`Copy` value cannot be moved out from under an array's element
  either: an array ends its elements whole, so a move out of one is
  refused until the drop walk can name an element. A value without a
  destructor is read out by ordinary copy.
- A non-`Copy` value cannot be moved out of a reference: reading `*r`
  as a value, taking a field out of the referent, and moving a pattern's
  payload out are all refused. A pattern over a borrowed value reads the
  discriminant in place and binds what it takes behind a reference, so
  `match *e { E::A(s) => s.len(), E::B => 0 }` reads the payload without
  taking it; a `Copy` payload is a `&T` like any other, so a value needs
  the `*` the reference is. The reference one of the
  compiler's own `unsafe` intrinsics hands out - the allocator's
  accessors - names storage the caller owns, so reading through one is
  a move; an ordinary `unsafe` function returns a borrow like any
  other, and ADR-0050 keeps the move rule for it.
- A closure's capture list declares what its environment holds: a
  bare name by value (a move, or a copy for a Copy type), `&name` as
  a shared borrow, and `&mut name` as an exclusive borrow. The
  borrow lasts while any handle of the closure value is live, and a
  value reached through a capture is borrowed content: it cannot be
  moved out of.
- These rules make safety checking independent of general may-alias
  analysis. Optimization-time alias analysis in backends is unaffected
  and still exists.
- `&mut T` fields are permitted and make the aggregate move-only
  through the structural `Copy` rule (see region composition below).
- A buffer view (`str`, `&[T]`) carries the loan of the buffer it
  reads, so a write that may reallocate or overwrite the buffer
  conflicts while the view is live. Copying the view keeps the loan;
  copying out its length does not, because the length half owns
  nothing.

## Region model

- A region is the set of program points at which a reference is
  required to be valid. Region relations are outlives constraints over
  a partial order; there is no disjunction in the constraint language
  (no user syntax can express region formulas).
- Conditional joins are conjunctions: a reference that may originate
  from either of two inputs is valid only where both inputs are valid
  (`ret ⊆ r_a ∩ r_b`). Callers keep all contributing inputs alive.
- Structs containing `&` fields compose by intersection:
  `Region(S) = ∩ Region(field_i)`. Struct arguments contribute their
  fields' invisible region parameters, expanded automatically.

## Inference strategy

- Intraprocedural inference is classical dataflow over the control-flow
  graph (dense bitsets, post-order fixed-point iteration). Loans are
  invalidated by moves, exclusive borrows, and assignments.
- Interprocedural reasoning is expressed exclusively through function
  summaries (see `summaries.md`). MVP computes summaries in bounded
  sweeps over the call graph; exceeding the bound is an internal
  compiler error, never a silent precision loss.
- No lifetime annotations exist in the language. If annotations ever
  become necessary, they MUST take `where`-style outlives-bound form,
  never boolean formulas.
