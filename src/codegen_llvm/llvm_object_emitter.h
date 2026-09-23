// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string_view>
#include <vector>

#include "fpag/base/numeric.h"
#include "fpag/base/result.h"

namespace llvm {
class Module;
}  // namespace llvm

namespace codegen_llvm {

enum class ObjectEmitError : u8 {
  UnknownTriple,
  NoTargetMachine,
  CannotEmit,
  IoError,
};

// Emits a relocatable object file for the module. An empty triple
// selects the host. Only targets linked into the binary are
// available (host backends on native builds); anything else reports
// UnknownTriple without touching the module.
base::Result<std::vector<u8>, ObjectEmitError> emit_object(
    llvm::Module& module,
    std::string_view triple,
    bool optimize = false);

}  // namespace codegen_llvm
