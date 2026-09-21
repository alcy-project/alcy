// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#include "codegen_llvm/llvm_object_emitter.h"

#include <memory>
#include <string>
#include <string_view>
#include <system_error>

#include "cfg/build_config.h"
#include "codegen_llvm/common.h"
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

base::Result<void, ObjectEmitError> emit_object(llvm::Module& module,
                                                std::string_view triple,
                                                std::string_view output_path) {
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
  std::unique_ptr<llvm::TargetMachine> machine(target->createTargetMachine(
      triple_obj, "generic", "", options, llvm::Reloc::PIC_));
  if (machine == nullptr) {
    return base::make_err(ObjectEmitError::NoTargetMachine);
  }
  module.setDataLayout(machine->createDataLayout());
  std::error_code code;
  llvm::raw_fd_ostream output(std::string(output_path), code,
                              llvm::sys::fs::OF_None);
  if (code) {
    return base::make_err(ObjectEmitError::IoError);
  }
  llvm::legacy::PassManager passes;
  if (machine->addPassesToEmitFile(passes, output, nullptr,
                                   llvm::CodeGenFileType::ObjectFile)) {
    return base::make_err(ObjectEmitError::CannotEmit);
  }
  passes.run(module);
  output.flush();
  return base::make_ok();
}

}  // namespace codegen_llvm
