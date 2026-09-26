# ir

Intermediate representation: storage, builder, and verifier.

`Storage` is dense index-addressed tables (functions, blocks,
instructions, operands, registers, types and composite metadata).
`StorageBuilder` interns entries and hands out indices; `build()`
verifies and returns proof-carrying `VerifiedStorage`. `verify_storage`
is the structural checker both build-time and test-time use.

## Entry points

- `StorageBuilder::build()` ->
  `base::Result<VerifiedStorage, ir::VerifyError>`. Invalid builder
  output becomes a structured error instead of corrupt IR.
- `verify_storage(storage)` -> `VerifyResult`. Pure structural check.
- `VerifiedStorage` - move-only proof that verification ran. The
  constructor is private (`StorageBuilder` is the only factory);
  consumers that require valid IR (`LoweredPackage::storage`, the
  emitter) hold the proof instead of re-verifying. `unwrap()` moves
  the storage out and consumes the proof.

## Input requirements

- Builder setter preconditions (e.g. `ref_type` on a live index) are
  internal invariants held by construction; cross-table consistency
  is enforced once, at `build()`.
- Passes must take `VerifiedStorage` (or types chopped from one)
  rather than re-running the verifier; the emitter documents this
  provenance on its constructor.
