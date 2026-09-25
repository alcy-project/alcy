# Function Summaries and Lifetime Synthesis (MVP subset)

Lifetime synthesis is the thesis: the compiler derives interprocedural
region relations automatically, so callers never write annotations.

## Summary language

A summary maps input regions to an output region expression:

```
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
  beyond the borrow rules exists in MVP.
- Reborrowing freezes the source place for the derived reference's
  lifetime; freezing is enforced by the exclusivity rule (loan
  invalidation), not by extra summary machinery.

## Call-site instantiation

- At a call site, actual argument regions are substituted for the
  summary parameters and outlives constraints are checked. There is no
  search and no backtracking: instantiation is decidable and fast.
- Struct arguments expand invisible region parameters automatically.

## Staging

- **MVP**: summaries over the constructors above; bounded fixed-point
  iteration for (mutual) recursion; no closures, no spec objects,
  no higher-ranked region polymorphism beyond struct projection.
- **Post-MVP**: demand-driven summary computation on the parallel
  engine, full-precision mutual recursion, two-phase borrows, and any
  precision refinements the MVP subset cannot express.
