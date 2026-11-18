# Function Summaries and Lifetime Synthesis

Lifetime synthesis is the thesis: the compiler derives interprocedural
region relations automatically, so callers never write annotations.

## Summary language

A summary maps input regions to an output region expression:

```text
summary f(r1..rn) -> R
R ::= ri                    # identity
    | R ∩ R                 # conditional joins, struct composition
    | proj(R, field_path)   # projection through struct fields
    | static                # statically stored origins
```

- Shared (`&`) returns admit arbitrary combinations of the above.
- Exclusive (`&mut`) returns are flow-checked like any other return:
  the summary records whichever inputs may flow out, and call-site
  instantiation keeps those inputs alive. No synthesis restriction
  beyond the borrow rules exists.
- Reborrowing freezes the source place for the derived reference's
  lifetime; freezing is enforced by the exclusivity rule (loan
  invalidation), not by extra summary machinery.

## Call-site instantiation

- At a call site, actual argument regions are substituted for the
  summary parameters and outlives constraints are checked. There is no
  search and no backtracking: instantiation is decidable and fast.
- Struct arguments expand invisible region parameters automatically.

## Generic bodies cross checked

A generic item's body is checked once, at its declaration, against its
own parameters: a use instantiates the checked body and does not check
it again. The IR names a type parameter - a type node carries the
index of the parameter of the item it belongs to - and a generic item
is emitted once, in that parametric form, as a function no backend
receives.

Specialization is a pass over IR that runs before the borrow pass, in
each package's own build. It resolves the calls a parametric body
recorded, substitutes the concrete type arguments, and clones the
body, memoized by item and arguments, so the borrow pass sees only
monomorphic code and a generic body is borrowed once per
specialization. One pass serves both halves: a package's own generics
and a dependent's uses go through it, because the AST-level
instantiation walk is gone.

A summary carries a generic body in its parametric form, and the
dependent spells the row's declaration, so the specialization
produces the instance beside the dependent's own code. A monomorphic
body is not carried - the dependent links the library's object for it.

An item the once-check cannot stand in for (a value parameter, a
`comp fn`, an owning `fn()` slot, a parameter bounded by the `Fn` spec)
keeps a declaration-only row: a dependent that calls one is told the
item is unavailable rather than reading a body the library could not
carry.

## Scope

- Summaries cover the constructors above, with bounded fixed-point
  iteration for (mutual) recursion; indirect calls through closures
  carry their arguments' and callee's loans rather than a summary; no
  spec objects, and no higher-ranked region polymorphism beyond struct
  projection.
- Remaining work, tracked in `deferred.md`: demand-driven summary
  computation on the parallel engine, full-precision mutual recursion,
  two-phase borrows, and any further precision refinements.
