// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#pragma once

#include <string_view>

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
base::Result<void, ObjectEmitError> emit_object(llvm::Module& module,
                                                std::string_view triple,
                                                std::string_view output_path);

}  // namespace codegen_llvm
