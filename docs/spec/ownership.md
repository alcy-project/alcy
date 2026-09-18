# Ownership and Regions (MVP)

## Borrow rules

- `&T` (shared reference): freely duplicable; any number may coexist.
- `&mut T` (exclusive reference): at most one live at a time, and no
  `&T` to the same place may be live while it is.
- These rules make safety checking independent of general may-alias
  analysis. Optimization-time alias analysis in backends is unaffected
  and still exists.
- `&mut T` MUST NOT be stored in struct fields. `&T` fields are
  permitted (see region composition below).
- `&mut`-returning functions are restricted to identity and projection
  forms: the returned reference MUST be the input reference or a
  projection through its fields. No branching or synthesis over
  multiple inputs for exclusive returns.

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
  invalidated by moves, exclusive borrows, and scope exits.
- Interprocedural reasoning is expressed exclusively through function
  summaries (see `summaries.md`). MVP computes summaries in topological
  call-graph order with bounded fixed-point iteration for recursion;
  non-convergence is a compile-time error.
- No lifetime annotations exist in the language. If annotations ever
  become necessary, they MUST take `where`-style outlives-bound form,
  never boolean formulas.
