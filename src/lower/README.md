# lower

Lowering from `CheckedPackage` to `LoweredPackage` (IR plus side
tables for ownership analysis).

Every emitted instruction records its source span and every address
alloca records its bound name, so borrow checking can point at
source. `Lowerer` (in `lowerer.h`, included only by this module's
translation units) carries the lowering state; it is not public API.

## Entry points

- `lower_package(package, width, ast, strings, bag)` ->
  `base::Result<LoweredPackage, diag::Reported>`. The checked types
  arrive as `ir::VerifiedStorage` proof, so no entry re-verification
  runs; the finished IR is verified once by `StorageBuilder::build`
  on the way out (`LOWER_INTERNAL`).

## Input requirements

- `CheckedPackage` is consumed by move: lowering reseeds its builder
  from the checked types' proof and the package must not be used
  after.
- A `Lowerer` that set `failed` yields no package; diagnostics for
  the failure are already in the bag.
