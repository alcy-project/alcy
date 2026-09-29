// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "benchmarks/case_codegen.h"

#include <memory>
#include <utility>

#include "benchmarks/clock.h"
#include "benchmarks/fixture.h"
#include "benchmarks/generator.h"
#include "benchmarks/runner.h"
#include "benchmarks/sink.h"
#include "codegen_llvm/common.h"
#include "codegen_llvm/llvm_ir_emitter.h"
#include "codegen_llvm/llvm_object_emitter.h"
#include "config/build_config.h"
#include "ir/type.h"

namespace bench {

namespace {

// Emitting is milliseconds, and the object case is tens of them, so its
// floor is longer: at one second it would gather twenty samples, which
// is not enough for a median to mean anything and the engine would say
// so on every run.
constexpr MeasurementPolicy EMIT = {
    .warmup = 3,
    .samples = 0,
    .min_duration_ns = 1000ull * 1000 * 1000,
    .min_batch_ns = 0,
};

constexpr MeasurementPolicy EMIT_OBJECT = {
    .warmup = 2,
    .samples = 0,
    .min_duration_ns = 4000ull * 1000 * 1000,
    .min_batch_ns = 0,
};

}  // namespace

void run_codegen_cases(Runner<SteadyClock>& runner,
                       const SourceSpec& spec,
                       const CaseFilter& filter,
                       Emitter& emit) {
  // LLVM owns a context, and the emitter consumes the storage it is
  // given, so every sample needs a package lowered again and a module
  // built on the same context.
  llvm::LLVMContext context;
  std::unique_ptr<CompilerFixture> fixture;

  // A pointer rather than an optional: `rebuild` is what puts it there,
  // and a unique_ptr reads without the question of whether it is set.
  const auto rebuild = [&] {
    fixture = std::make_unique<CompilerFixture>(spec);
    fixture->resolve();
    fixture->analyze();
    fixture->lower();
  };

  // The IR is printed, not emitted: `emit_ir` is a print, and the case
  // is the cost of handing a module to a reader.
  if (filter.wants(CaseId{"backend", "emit-ir"})) {
    emit(CaseId{"backend", "emit-ir"}, EMIT, runner.measure(EMIT, rebuild, [&] {
      auto module = std::make_unique<llvm::Module>("bench", context);
      codegen_llvm::LlvmIrEmitter emitter(module.get(), fixture->take_storage(),
                                          &fixture->strings(),
                                          ir::PointerWidth::W64);
      std::move(emitter).emit();
      static_cast<void>(codegen_llvm::emit_ir(*module));
    }));
  }

#if !BUILD_FLAG(IS_OS_ASMJS)
  // The object is written into a buffer rather than to a file, so
  // nothing here is a write to the filesystem: the target machine's
  // code generation and the object it produces, nothing else.
  if (filter.wants(CaseId{"backend", "emit-object"})) {
    // Emitting re-registers the linked targets, and that registration is
    // the larger half of the call: measured cold, a sample here costs
    // about 165ms, of which roughly 120 is the registry and 45 is the
    // code generation the case is about. The registry is process state
    // rather than work done for a module, so it is paid once here instead
    // of inside every sample.
    rebuild();
    {
      auto warm = std::make_unique<llvm::Module>("warm", context);
      codegen_llvm::LlvmIrEmitter emitter(warm.get(), fixture->take_storage(),
                                          &fixture->strings(),
                                          ir::PointerWidth::W64);
      std::move(emitter).emit();
      static_cast<void>(codegen_llvm::emit_object(*warm, ""));
    }
    emit(CaseId{"backend", "emit-object"}, EMIT_OBJECT,
         runner.measure(EMIT_OBJECT, rebuild, [&] {
           auto module = std::make_unique<llvm::Module>("bench", context);
           codegen_llvm::LlvmIrEmitter emitter(
               module.get(), fixture->take_storage(), &fixture->strings(),
               ir::PointerWidth::W64);
           std::move(emitter).emit();
           static_cast<void>(codegen_llvm::emit_object(*module, ""));
         }));
  }
#endif  // !BUILD_FLAG(IS_OS_ASMJS)
}

}  // namespace bench
