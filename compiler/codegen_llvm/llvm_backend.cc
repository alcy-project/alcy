// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "codegen_llvm/llvm_backend.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "codegen_llvm/common.h"
#include "codegen_llvm/target.h"
#include "config/build_config.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/debug/process_id.h"
#include "fpag/debug/profiler/profile_event.h"
#include "fpag/debug/thread_id.h"
#include "fpag/debug/time_util.h"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
// New pass manager pieces name parameters -Wall flags under -Werror;
// same treatment as other third-party headers in this codebase.
#include "llvm/Analysis/CGSCCPassManager.h"
#include "llvm/Analysis/LoopAnalysisManager.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/OptimizationLevel.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/StandardInstrumentations.h"
#pragma clang diagnostic pop
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/Support/raw_ostream.h"

namespace codegen_llvm {

namespace {

void init_linked_targets() {
#if !BUILD_FLAG(IS_OS_ASMJS)
  ::LLVMInitializeX86TargetInfo();
  ::LLVMInitializeX86Target();
  ::LLVMInitializeX86TargetMC();
  ::LLVMInitializeX86AsmPrinter();

  ::LLVMInitializeAArch64TargetInfo();
  ::LLVMInitializeAArch64Target();
  ::LLVMInitializeAArch64TargetMC();
  ::LLVMInitializeAArch64AsmPrinter();

  ::LLVMInitializeRISCVTargetInfo();
  ::LLVMInitializeRISCVTarget();
  ::LLVMInitializeRISCVTargetMC();
  ::LLVMInitializeRISCVAsmPrinter();
#endif

  ::LLVMInitializeWebAssemblyTargetInfo();
  ::LLVMInitializeWebAssemblyTarget();
  ::LLVMInitializeWebAssemblyTargetMC();
  ::LLVMInitializeWebAssemblyAsmPrinter();

  // TODO: add all targets
  // ::LLVMInitializeAMDGPUTargetInfo();
  // ::LLVMInitializeAMDGPUTarget();
  // ::LLVMInitializeAMDGPUTargetMC();
  // ::LLVMInitializeAMDGPUAsmPrinter();

  // ::LLVMInitializeARMTargetInfo();
  // ::LLVMInitializeARMTarget();
  // ::LLVMInitializeARMTargetMC();
  // ::LLVMInitializeARMAsmPrinter();

  // ::LLVMInitializeAVRTargetInfo();
  // ::LLVMInitializeAVRTarget();
  // ::LLVMInitializeAVRTargetMC();
  // ::LLVMInitializeAVRAsmPrinter();

  // ::LLVMInitializeBPFTargetInfo();
  // ::LLVMInitializeBPFTarget();
  // ::LLVMInitializeBPFTargetMC();
  // ::LLVMInitializeBPFAsmPrinter();

  // ::LLVMInitializeHexagonTargetInfo();
  // ::LLVMInitializeHexagonTarget();
  // ::LLVMInitializeHexagonTargetMC();
  // ::LLVMInitializeHexagonAsmPrinter();

  // ::LLVMInitializeLanaiTargetInfo();
  // ::LLVMInitializeLanaiTarget();
  // ::LLVMInitializeLanaiTargetMC();
  // ::LLVMInitializeLanaiAsmPrinter();

  // ::LLVMInitializeLoongArchTargetInfo();
  // ::LLVMInitializeLoongArchTarget();
  // ::LLVMInitializeLoongArchTargetMC();
  // ::LLVMInitializeLoongArchAsmPrinter();

  // ::LLVMInitializeMipsTargetInfo();
  // ::LLVMInitializeMipsTarget();
  // ::LLVMInitializeMipsTargetMC();
  // ::LLVMInitializeMipsAsmPrinter();

  // ::LLVMInitializeNVPTXTargetInfo();
  // ::LLVMInitializeNVPTXTarget();
  // ::LLVMInitializeNVPTXTargetMC();
  // ::LLVMInitializeNVPTXAsmPrinter();

  // ::LLVMInitializePowerPCTargetInfo();
  // ::LLVMInitializePowerPCTarget();
  // ::LLVMInitializePowerPCTargetMC();
  // ::LLVMInitializePowerPCAsmPrinter();

  // ::LLVMInitializeSparcTargetInfo();
  // ::LLVMInitializeSparcTarget();
  // ::LLVMInitializeSparcTargetMC();
  // ::LLVMInitializeSparcAsmPrinter();

  // ::LLVMInitializeSystemZTargetInfo();
  // ::LLVMInitializeSystemZTarget();
  // ::LLVMInitializeSystemZTargetMC();
  // ::LLVMInitializeSystemZAsmPrinter();
}

}  // namespace

namespace {

// The machine `target` names. Both entry points below need one, and both
// fail the same way.
base::Result<std::unique_ptr<llvm::TargetMachine>, ObjectEmitError>
target_machine(const Target& target) {
  init_linked_targets();
  const llvm::Triple triple(target.triple);
  std::string error;
  const llvm::Target* backend =
      llvm::TargetRegistry::lookupTarget(triple, error);
  if (backend == nullptr) {
    return base::make_err(ObjectEmitError::UnknownTriple);
  }
  llvm::TargetOptions options;
  std::unique_ptr<llvm::TargetMachine> machine(backend->createTargetMachine(
      triple, "generic", "", options, llvm::Reloc::PIC_, std::nullopt));
  if (machine == nullptr) {
    return base::make_err(ObjectEmitError::NoTargetMachine);
  }
  return base::make_ok(std::move(machine));
}

}  // namespace

base::Result<void, ObjectEmitError> configure_target(llvm::Module& module,
                                                     const Target& target) {
  base::Result<std::unique_ptr<llvm::TargetMachine>, ObjectEmitError> machine =
      target_machine(target);
  if (machine.is_err()) {
    return base::make_err(std::move(machine).unwrap_err());
  }
  module.setTargetTriple(llvm::Triple(target.triple));
  module.setDataLayout(std::move(machine).unwrap()->createDataLayout());
  return base::make_ok();
}

base::Result<void, ObjectEmitError> optimize_module(llvm::Module& module,
                                                    const Target& target,
                                                    debug::Profiler* profiler) {
  base::Result<void, ObjectEmitError> configured =
      configure_target(module, target);
  if (configured.is_err()) {
    return base::make_err(std::move(configured).unwrap_err());
  }
  base::Result<std::unique_ptr<llvm::TargetMachine>, ObjectEmitError> machine =
      target_machine(target);
  if (machine.is_err()) {
    return base::make_err(std::move(machine).unwrap_err());
  }
  llvm::LoopAnalysisManager lam;
  llvm::FunctionAnalysisManager fam;
  llvm::CGSCCAnalysisManager cgam;
  llvm::ModuleAnalysisManager mam;
  llvm::PassInstrumentationCallbacks pic;
  llvm::StandardInstrumentations si(module.getContext(), false);
  si.registerCallbacks(pic, &mam);
  // Each pass run gets its own event under "llvm-pass", named for the
  // pass: an O3 pipeline is hundreds of runs, and the cheapest part of a
  // release build hides there.
  //
  // The callbacks fire on the thread the pass runs on, and the events
  // land there too only because passes run there for now: a pass that
  // moved work elsewhere would show as the pass that moved it. Scopes
  // are not movable, so no stack of them fits in a callback; the stack
  // therefore holds interned names and start times, and each exit builds
  // its event from the entry below.
  //
  // Times come from `debug::current_timestamp_ns`, the clock
  // `ProfileSection` reads: an event recorded against another epoch
  // would nest under nothing and read as a root.
  if (profiler != nullptr) {
    // A thread-local stack of entries, because passes nest: a module
    // pass runs function passes, which run loop passes. One entry per
    // before-callback, popped by the matching after-callback, is what
    // makes the nesting read in a trace viewer.
    struct PassTimer {
      explicit PassTimer(debug::Profiler* profiler) : profiler(profiler) {}
      void push(llvm::StringRef name) {
        // `runBeforePass` hands the string to the callback, so interning
        // it here keeps the pool the events resolve into under the
        // profiler rather than under the pass.
        stack.push_back({profiler->intern(std::string(name)),
                         ::debug::current_timestamp_ns()});
      }
      void pop() {
        if (stack.empty()) {
          return;
        }
        const Pending pending = stack.back();
        stack.pop_back();
        debug::ProfileEvent event;
        event.name = pending.name;
        event.category = profiler->intern("llvm-pass");
        event.start_time_ns = pending.start_ns;
        event.duration_ns = ::debug::current_timestamp_ns() - pending.start_ns;
        event.thread_id = ::debug::current_thread_id();
        event.process_id = ::debug::current_process_id();
        profiler->record_event(event);
      }
      struct Pending {
        str::StringPoolId name = str::INVALID_STRING_POOL_ID;
        u64 start_ns = 0;
      };
      debug::Profiler* profiler;
      // Nested passes are shallow, so a heap vector holds the whole stack
      // without mattering; passes run rarely enough that one small
      // allocation per optimize is noise against the pipeline itself.
      std::vector<Pending> stack;
    };
    // The passes below run inside this call, so the timer can be a
    // local; a thread-local kept the first profiler it saw and reported
    // a later run's passes through a stale pointer.
    PassTimer timer{profiler};
    pic.registerBeforeNonSkippedPassCallback(
        [&timer](llvm::StringRef name, const llvm::Any&) { timer.push(name); });
    pic.registerAfterPassCallback(
        [&timer](llvm::StringRef, const llvm::Any&,
                 const llvm::PreservedAnalyses&) { timer.pop(); });
  }
  // The machine outlives the builder: unwrap() moves into a temporary,
  // so taking .get() off it would dangle past this statement.
  std::unique_ptr<llvm::TargetMachine> owned = std::move(machine).unwrap();
  llvm::PassBuilder pb(owned.get(), llvm::PipelineTuningOptions(), std::nullopt,
                       &pic);
  pb.registerModuleAnalyses(mam);
  pb.registerCGSCCAnalyses(cgam);
  pb.registerFunctionAnalyses(fam);
  pb.registerLoopAnalyses(lam);
  pb.crossRegisterProxies(lam, fam, cgam, mam);
  llvm::ModulePassManager mpm =
      pb.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O3);
  mpm.run(module, mam);
  return base::make_ok();
}

base::Result<std::vector<u8>, ObjectEmitError> emit_object(
    llvm::Module& module,
    const Target& target) {
  base::Result<void, ObjectEmitError> configured =
      configure_target(module, target);
  if (configured.is_err()) {
    return base::make_err(std::move(configured).unwrap_err());
  }
  base::Result<std::unique_ptr<llvm::TargetMachine>, ObjectEmitError> machine =
      target_machine(target);
  if (machine.is_err()) {
    return base::make_err(std::move(machine).unwrap_err());
  }
  // The passes below borrow the machine; it must outlive passes.run.
  std::unique_ptr<llvm::TargetMachine> owned = std::move(machine).unwrap();
  llvm::SmallVector<char, 0> buffer_vec;
  llvm::raw_svector_ostream output(buffer_vec);
  llvm::legacy::PassManager passes;
  if (owned->addPassesToEmitFile(passes, output, nullptr,
                                 llvm::CodeGenFileType::ObjectFile)) {
    return base::make_err(ObjectEmitError::CannotEmit);
  }
  passes.run(module);

  std::vector<u8> result(buffer_vec.begin(), buffer_vec.end());
  return base::make_ok(std::move(result));
}

std::string emit_ir(llvm::Module& module) {
  std::string text;
  llvm::raw_string_ostream out(text);
  // Not a new pass pipeline: the module is already built, and printing it
  // must not change it. LLVM's own -S does the same.
  module.print(out, nullptr);
  out.flush();
  return text;
}

std::vector<u8> emit_bitcode(llvm::Module& module) {
  llvm::SmallVector<char, 0> buffer;
  llvm::raw_svector_ostream out(buffer);
  llvm::WriteBitcodeToFile(module, out);
  std::vector<u8> result(buffer.begin(), buffer.end());
  return result;
}

}  // namespace codegen_llvm
