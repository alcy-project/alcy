# ADR-0043: One value says what is being built for

- Subject: the compiler
- Status: Accepted
- Date: 2026-10-03

## Context

Three answers to "what machine is this for", and two of them were about
the machine the compiler was built on rather than the one it was building
for:

- `llvm_object_emitter.cc` chose the architecture, the OS, the
  relocation model, and the CPU: it registered four backends, asked for
  the host's triple when the caller passed none, and created every target
  machine with `"generic"` and `Reloc::PIC_`.
- `pipeline/target.h` chose the pointer width, as `TARGET_WIDTH`, and
  handed it separately to the analyzer, lowering, and the emitter.
- `runtime_ir.cc` chose its libc bindings - `write` and `posix_memalign`
  against `_write` and `_aligned_malloc` - from `BUILD_FLAG(IS_OS_WIN)`,
  which is the OS the compiler was compiled for.

The module's data layout was the fourth problem, and the one with a
consequence. `setDataLayout` happened inside `prepare_module`, which ran
*after* `LlvmIrEmitter` in every path. So `TypeSizeOf`, `TypeAlignOf`, and
the enum-slot check all read LLVM's default layout - the host's - and the
check that would have caught it was written to skip itself:

```cpp
if (!module_->getDataLayout().isDefault()) { ... }
```

Which means it never ran. A layout applied after emission is a layout that
describes nothing about what was emitted.

## Decision

`codegen_llvm::Target` is the machine: a triple and a pointer width. The
pipeline decides it once, on `PipelineContext`, and hands the same value
to the analyzer, lowering, the emitter, the runtime, and the object
writer. `host_triple()` answers the host's; a `--target` flag will
replace that one call site.

`configure_target(module, target)` sets the module's triple and data
layout, and the pipeline calls it before `LlvmIrEmitter`. Emission and
optimization configure the module themselves when they are called
directly, so the entry points stay usable alone; the emitter requires the
layout and checks the enum type it builds against it unconditionally.

`add_runtime_definitions(module, target)` takes the target, so the libc
bindings follow the target's OS. A build that names a Windows triple gets
`_write` whether or not the compiler was compiled for Windows.

`llvm_object_emitter.{h,cc}` is `llvm_backend.{h,cc}`: optimization, IR
printing, and bitcode writing were never about object emission, and a
reader looking for the target machine had no reason to open a file named
after one of its four jobs.

## Consequences

- The unconditional check found a bug the skipped one had hidden.
  `ir::type_layout`'s enum case rounded the discriminant up to the
  payload area's alignment and then stopped, so `enum E { A(u8) }` was
  five bytes while the LLVM struct `{i32, [1 x i8]}` allocates eight. An
  array of such an enum strides by the layout size, so every element
  after the first sat where the emitted type has padding. It is fixed in
  the same change, and `type_layout_test.cc` covers the byte payload and
  the array of them.
- The layout the compiler computes and the layout LLVM lays out are now
  checked against each other on every debug build, which is the only place
  the two can disagree without a miscompile being visible first.
- `ObjectEmitError::IoError` goes: nothing in the module ever produced it,
  and the pipeline reports file errors with its own code.
- Cross-compilation is still not testable here, but it is now a value
  rather than a set of host facts, which is what a test would need.
- Two things stay open, both recorded in `compiler/codegen_llvm/README.md`:
  the optimization level is still O3 behind a boolean with no `-O`, and
  `pipeline` still holds `llvm::LLVMContext` and `llvm::Module` itself.
