// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "codegen_llvm/emit.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "codegen/backend.h"
#include "codegen/target.h"
#include "codegen_llvm/common.h"
#include "codegen_llvm/llvm_backend.h"
#include "codegen_llvm/llvm_ir_emitter.h"
#include "codegen_llvm/runtime_ir.h"
#include "config/build_config.h"
#include "debug/dlog.h"
#include "debug/fatal.h"
#include "fpag/base/result.h"
#include "fpag/debug/profiler/profile_scope.h"
#include "ir/storage.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"

namespace codegen_llvm {
namespace {

// The module a request becomes, and the context it lives in.
//
// The two travel together because a module is invalid once its context
// goes, and the context is neither copyable nor movable, so the caller
// constructs this and has it filled rather than receiving one back.
struct EmittedModule {
  llvm::LLVMContext context;
  std::unique_ptr<llvm::Module> module;

  // Builds the module - the program and, defined in it, the runtime -
  // and, when asked for it, optimizes it. This is the only place that
  // decides: the optimization belongs to the module, not to whichever
  // output kind is written from it, and a caller that skipped it would
  // have every consumer reading unoptimized IR.
  base::Result<void, codegen::EmitError> build(codegen::EmitRequest& request) {
    module = std::make_unique<llvm::Module>("alcy_module", context);
    // Before the emitter, not after: the emitter asks the layout for
    // `TypeSizeOf` and `TypeAlignOf`, and a module that still carries
    // LLVM's default layout answers those for the host rather than for
    // the target.
    if (configure_target(*module, request.target).is_err()) {
      return base::make_err(codegen::EmitError::UnknownTarget);
    }
    // Entry synthesis wraps a `main` for binaries only; a library object
    // carries its items unwrapped, even one named `main`.
    LlvmIrEmitter emitter(module.get(), std::move(request.storage),
                          request.strings, request.target, request.emit_entry,
                          request.freestanding, request.profiler,
                          request.backtrace);
    std::vector<BacktraceName> visible = std::move(emitter).emit();
    // Before the optimizer, so the runtime is inlined and folded like
    // any other code, and after the program, so its definitions land in
    // the declarations the program's call sites already hold.
    {
      PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(request.profiler, "runtime",
                                               "backend");
      add_runtime_definitions(*module, request.target, request.freestanding,
                              request.backtrace);
    }
    if (request.backtrace) {
      // The frames the walk reads and the names it prints: every
      // generated function keeps a frame pointer, and the table names
      // the program's. Tail calls are off so the panic's own frame stays
      // a frame of its own; an inlined caller still has no frame to
      // find, which is the best-effort part. All three are what an
      // on-panic profile asked for and absent when it did not.
      define_backtrace_table(*module, visible);
      for (llvm::Function& function : *module) {
        if (!function.isDeclaration()) {
          function.addFnAttr("frame-pointer", "all");
          function.addFnAttr("disable-tail-calls", "true");
        }
      }
    }
#if BUILD_FLAG(IS_DEBUG)
    // The emitter checked its own output above; the runtime landed
    // after that check, so the module is rechecked with it in.
    if (llvm::verifyModule(*module, &llvm::errs())) [[unlikely]] {
      DLOG("LLVM verify module with runtime failed");
      module->print(llvm::errs(), nullptr);
      UNREACHABLE();
    }
#endif
    if (!request.optimize) {
      return base::make_ok();
    }
    PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(request.profiler, "optimize",
                                             "backend");
    if (optimize_module(*module, request.target, request.profiler).is_err()) {
      return base::make_err(codegen::EmitError::CannotOptimize);
    }
    return base::make_ok();
  }
};

}  // namespace

bool Llvm::supports(codegen::OutputKind kind, const codegen::Target& target) {
  // Whether the linked LLVM has the target's backend is answered when the
  // target machine is resolved; a target it does not know fails emission
  // with UnknownTarget, which is a better message than a silent false.
  (void)target;
  return kind != codegen::OutputKind::Module;
}

base::Result<std::vector<u8>, codegen::EmitError> Llvm::emit(
    codegen::EmitRequest request) {
  if (!supports(request.kind, request.target)) {
    return base::make_err(codegen::EmitError::Unsupported);
  }
  EmittedModule emitted;
  base::Result<void, codegen::EmitError> built = emitted.build(request);
  if (built.is_err()) {
    return base::make_err(std::move(built).unwrap_err());
  }
  switch (request.kind) {
    case codegen::OutputKind::Object: {
      PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(request.profiler, "emit-object",
                                               "backend");
      base::Result<std::vector<u8>, ObjectEmitError> object =
          emit_object(*emitted.module, request.target);
      if (object.is_err()) {
        return base::make_err(codegen::EmitError::CannotEmit);
      }
      return base::make_ok(std::move(object).unwrap());
    }
    case codegen::OutputKind::Text: {
      PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(request.profiler, "emit-ir",
                                               "backend");
      const std::string ir = emit_ir(*emitted.module);
      return base::make_ok(std::vector<u8>(ir.begin(), ir.end()));
    }
    case codegen::OutputKind::Bitcode: {
      PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(request.profiler, "emit-bitcode",
                                               "backend");
      return base::make_ok(emit_bitcode(*emitted.module));
    }
    case codegen::OutputKind::Module: break;
  }
  return base::make_err(codegen::EmitError::Unsupported);
}

}  // namespace codegen_llvm
