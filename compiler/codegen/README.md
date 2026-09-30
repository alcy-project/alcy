# codegen

Native backend experiments (currently `x86_emitter.cc`).

Not on the MVP pipeline path, which emits through LLVM
(`codegen_llvm`). Anything here must meet the same contract before
it wires in: consume verified IR, return `base::Result`, report
through the bag.
