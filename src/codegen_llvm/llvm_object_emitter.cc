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

base::Result<std::vector<u8>, ObjectEmitError>
emit_object(llvm::Module& module, std::string_view triple, bool optimize) {
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
  llvm::SmallVector<char, 0> buffer_vec;
  llvm::raw_svector_ostream output(buffer_vec);
  llvm::legacy::PassManager passes;
  if (machine->addPassesToEmitFile(passes, output, nullptr,
                                   llvm::CodeGenFileType::ObjectFile)) {
    return base::make_err(ObjectEmitError::CannotEmit);
  }
  passes.run(module);

  std::vector<u8> result(buffer_vec.begin(), buffer_vec.end());
  return base::make_ok(std::move(result));
}

}  // namespace codegen_llvm
