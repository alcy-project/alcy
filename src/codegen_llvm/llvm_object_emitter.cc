// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "codegen_llvm/llvm_object_emitter.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "codegen_llvm/common.h"
#include "config/build_config.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
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

// Prepares the module for the triple (empty selects the host) and
// returns its machine, tuned for the mode. Emission and optimization
// share it so a bad triple fails identically on both paths.
base::Result<std::unique_ptr<llvm::TargetMachine>, ObjectEmitError>
prepare_module(llvm::Module& module, std::string_view triple, bool optimize) {
  init_linked_targets();
  const std::string target_triple = triple.empty()
                                        ? llvm::sys::getDefaultTargetTriple()
                                        : std::string(triple);
  const llvm::Triple triple_obj(target_triple);
  std::string error;
  const llvm::Target* target =
      llvm::TargetRegistry::lookupTarget(triple_obj, error);
  if (target == nullptr) {
    return base::make_err(ObjectEmitError::UnknownTriple);
  }
  module.setTargetTriple(triple_obj);
  llvm::TargetOptions options;
  const llvm::CodeGenOptLevel opt_level =
      optimize ? llvm::CodeGenOptLevel::Aggressive
               : llvm::CodeGenOptLevel::Default;
  std::unique_ptr<llvm::TargetMachine> machine(
      target->createTargetMachine(triple_obj, "generic", "", options,
                                  llvm::Reloc::PIC_, std::nullopt, opt_level));
  if (machine == nullptr) {
    return base::make_err(ObjectEmitError::NoTargetMachine);
  }
  module.setDataLayout(machine->createDataLayout());
  return base::make_ok(std::move(machine));
}

base::Result<void, ObjectEmitError> optimize_module(llvm::Module& module,
                                                    std::string_view triple) {
  base::Result<std::unique_ptr<llvm::TargetMachine>, ObjectEmitError> machine =
      prepare_module(module, triple, true);
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

base::Result<std::vector<u8>, ObjectEmitError>
emit_object(llvm::Module& module, std::string_view triple, bool optimize) {
  if (optimize) {
    base::Result<void, ObjectEmitError> optimized =
        optimize_module(module, triple);
    if (optimized.is_err()) {
      return base::make_err(std::move(optimized).unwrap_err());
    }
  }
  base::Result<std::unique_ptr<llvm::TargetMachine>, ObjectEmitError> machine =
      prepare_module(module, triple, optimize);
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

}  // namespace codegen_llvm
