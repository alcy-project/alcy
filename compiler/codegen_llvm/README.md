# codegen_llvm

LLVM emission for verified alcy IR: one package to an `llvm::Module`, and
that module to an object, textual IR, or bitcode.

`Target` says what is being built for. It is one value - the triple and
the pointer width - because the emitter, the optimizer, the object writer,
and the runtime all have to agree about the machine, and they used to be
three answers to one question. `configure_target(module, target)` sets the
module's triple and data layout, and must run before `LlvmIrEmitter`: the
emitter reads the layout for `TypeSizeOf` and `TypeAlignOf`, and a module
that still carries LLVM's default layout answers those for the host.

## Entry points

- `Target`, `host_triple()`, `configure_target(module, target)`.
- `LlvmIrEmitter(module, storage, interner, target, emit_entry)` +
  `emit()`. `storage` is `ir::VerifiedStorage` proof: the emitter never
  sees unverified IR, and the unreachable opcode/shape cases rely on it.
  The only way to build a proof is `StorageBuilder::build`.
- `optimize_module(module, target)` runs the middle-end pipeline;
  `emit_object(module, target)` writes a relocatable object buffer into
  memory, `emit_ir` and `emit_bitcode` the module in the two forms a tool
  outside the compiler can read.
- `add_runtime_definitions(module, target, freestanding)` defines the
  `alcy_*` functions in the module, each only when the program
  declared it. The libc bindings they name follow the target's OS, so
  a Windows target gets `_write` and `_aligned_malloc` whoever is
  doing the building; a freestanding runtime reaches the kernel
  through raw syscalls instead.

## Input requirements

- The module is configured before the emitter runs; the emitter's enum
  path checks the type it builds against the module's data layout on
  every build rather than skipping the comparison.
- Debug builds recheck the *generated LLVM module* with
  `llvm::verifyModule`, not the alcy IR: re-running the alcy
  verifier would repeat build-time work.

## Not here yet

- The optimization level is fixed at O3 and reached through a boolean;
  there is no `-O`. A flag needs a value on `Target` or beside it, and the
  pipeline is the one deciding.
- `pipeline` still holds `llvm::LLVMContext` and `llvm::Module` itself,
  because emission hands the module back and forth. An opaque module
  handle owned here would keep LLVM out of the orchestrator.
