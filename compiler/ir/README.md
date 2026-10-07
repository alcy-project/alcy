# ir

Intermediate representation: storage, builder, verifier, and the name
table the storage's `StringPoolId`s resolve in.

`Storage` is dense index-addressed tables (functions, blocks,
instructions, operands, registers, types and composite metadata).
`StorageBuilder` interns entries and hands out indices; `build()`
verifies and returns proof-carrying `VerifiedStorage`. `verify_storage`
is the structural checker both build-time and test-time use.

## Entry points

- `StorageBuilder::build()` ->
  `base::Result<VerifiedStorage, ir::VerificationError>`. Invalid builder
  output becomes a structured error instead of corrupt IR.
- `verify_storage(storage)` -> `VerificationResult`. Pure structural check.
- `VerifiedStorage` - move-only proof that verification ran. The
  constructor is private (`StorageBuilder` is the only factory);
  consumers that require valid IR (`LoweredPackage::storage`, the
  emitter) hold the proof instead of re-verifying. `unwrap()` moves
  the storage out and consumes the proof.
- `SymbolTable::try_intern(name)` -> the handle for a name whose bytes
  outlive the table; `intern_copied` for one whose bytes do not;
  `get(id)` reads a name back. A name parsed out of a source is held
  as a view of the bytes that spell it, and a table that reaches its
  name budget is reported rather than left to exhaust memory.

## Input requirements

- Builder setter preconditions (e.g. `ref_type` on a live index) are
  internal invariants held by construction; cross-table consistency
  is enforced once, at `build()`.
- Passes must take `VerifiedStorage` (or types chopped from one)
  rather than re-running the verifier; the emitter documents this
  provenance on its constructor.
