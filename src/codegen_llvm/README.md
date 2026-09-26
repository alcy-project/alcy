# codegen_llvm

LLVM IR emission and object emission for verified alcy IR.

`LlvmIrEmitter` lowers one verified package to an `llvm::Module`;
`emit_object` assembles it to a relocatable object buffer.

## Entry points

- `LlvmIrEmitter(module, storage, interner, width)` + `emit()`.
  `storage` is `ir::VerifiedStorage` proof: the emitter never sees
  unverified IR, and the unreachable opcode/shape cases rely on it.
  The only way to build a proof is `StorageBuilder::build`.
- `emit_object(module, ...)` ->
  `base::Result<std::vector<u8>, ObjectEmitError>`: assembly
  failures are structured errors, reported by the pipeline as
  `PIPELINE_IO_ERROR`.

## Input requirements

- Debug builds recheck the *generated LLVM module* with
  `llvm::verifyModule`, not the alcy IR: re-running the alcy
  verifier would repeat build-time work.
