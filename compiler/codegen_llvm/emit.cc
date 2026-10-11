// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "codegen_llvm/emit.h"

#include <atomic>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/for_each.h"
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
#include "llvm/ADT/SmallVector.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/MemoryBufferRef.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/SplitModule.h"

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
                          request.freestanding, request.overflow_checks,
                          request.profiler, request.backtrace,
                          request.instr_spans, request.instr_site_names);
    EmittedBacktrace named = std::move(emitter).emit();
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
      // generated function keeps a frame pointer, and one table names
      // the program's functions while another names the call sites a
      // frame returns to. Tail calls are off so the panic's own frame
      // stays a frame of its own; an inlined caller still has no frame
      // to find, which is the best-effort part. All three are what an
      // on-panic profile asked for and absent when it did not.
      define_backtrace_table(*module, named.functions);
      define_callsite_table(*module, named.sites);
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

base::Result<std::vector<std::vector<u8>>, codegen::EmitError>
Llvm::emit_artifacts(codegen::EmitRequest request) {
  const u32 parts =
      request.kind == codegen::OutputKind::Object && request.parts > 1
          ? request.parts
          : 1;
  if (parts <= 1) {
    base::Result<std::vector<u8>, codegen::EmitError> single =
        Llvm::emit(std::move(request));
    if (single.is_err()) {
      return base::make_err(std::move(single).unwrap_err());
    }
    std::vector<std::vector<u8>> artifacts;
    artifacts.push_back(std::move(single).unwrap());
    return base::make_ok(std::move(artifacts));
  }
  if (!supports(request.kind, request.target)) {
    return base::make_err(codegen::EmitError::Unsupported);
  }
  // Built and optimized whole, then partitioned: the middle end sees one
  // module, so a release build keeps the cross-function inlining it has
  // today, and only code generation - which is per function either way - is
  // spread. `SplitModule` writes partitions a linker accepts, which is what
  // lets the caller link them back into one image.
  EmittedModule emitted;
  base::Result<void, codegen::EmitError> built = emitted.build(request);
  if (built.is_err()) {
    return base::make_err(std::move(built).unwrap_err());
  }
  std::vector<std::unique_ptr<llvm::Module>> split;
  split.reserve(parts);
  llvm::SplitModule(*emitted.module, parts,
                    [&split](std::unique_ptr<llvm::Module> part) {
                      split.push_back(std::move(part));
                    });
  // The parts come out of the source module's context, and code generation
  // wants a context per thread. They take the trip LLVM's own parallel code
  // generation takes: written to bitcode where they stand, and read back
  // into a context of their own by the thread that will use it. Carrying IR
  // across contexts by hand is not a path LLVM supports; this is.
  std::vector<std::vector<u8>> bitcode(split.size());
  std::vector<std::unique_ptr<llvm::LLVMContext>> contexts(split.size());
  for (usize at = 0; at < split.size(); ++at) {
    contexts[at] = std::make_unique<llvm::LLVMContext>();
    llvm::SmallVector<char, 0> buffer;
    llvm::raw_svector_ostream out(buffer);
    llvm::WriteBitcodeToFile(*split[at], out);
    bitcode[at].assign(buffer.begin(), buffer.end());
  }
  std::vector<std::vector<u8>> objects(split.size());
  std::atomic<bool> failed{false};
  const u32 workers = request.workers == 0 ? 1 : request.workers;
  base::for_each(0, bitcode.size(), workers, [&](usize at, usize worker) {
    (void)worker;
    const llvm::StringRef bytes(
        reinterpret_cast<const char*>(bitcode[at].data()), bitcode[at].size());
    llvm::Expected<std::unique_ptr<llvm::Module>> parsed =
        llvm::parseBitcodeFile(llvm::MemoryBufferRef(bytes, "alcy_part"),
                               *contexts[at]);
    if (!parsed) {
      llvm::consumeError(parsed.takeError());
      failed.store(true);
      return;
    }
    base::Result<std::vector<u8>, ObjectEmitError> object =
        emit_object(*parsed.get(), request.target);
    if (object.is_err()) {
      failed.store(true);
      return;
    }
    objects[at] = std::move(object).unwrap();
  });
  if (failed.load()) {
    return base::make_err(codegen::EmitError::CannotEmit);
  }
  return base::make_ok(std::move(objects));
}

}  // namespace codegen_llvm
