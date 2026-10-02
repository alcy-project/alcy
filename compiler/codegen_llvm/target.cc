// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "codegen_llvm/target.h"

#include <string>

#include "codegen_llvm/common.h"

namespace codegen_llvm {

bool Target::is_windows() const {
  return llvm::Triple(triple).isOSWindows();
}

std::string host_triple() {
  return llvm::sys::getDefaultTargetTriple();
}

}  // namespace codegen_llvm
