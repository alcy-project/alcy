// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <vector>

#include "codegen/backend.h"
#include "codegen/target.h"
#include "fpag/base/result.h"

namespace codegen_llvm {

// The LLVM backend: one emitter for every triple the linked target
// backends cover, and the only one that can print a module or run the
// middle end. It answers what `codegen::BackendImpl` asks of any
// implementation, and nothing here names an LLVM type.
struct Llvm {
  static constexpr codegen::Backend ID = codegen::Backend::Llvm;

  // LLVM has a module to print, so it answers every kind but a final
  // module; the linker turns its objects into executables.
  [[nodiscard]] static bool supports(codegen::OutputKind kind,
                                     const codegen::Target& target);

  static base::Result<std::vector<u8>, codegen::EmitError> emit(
      codegen::EmitRequest request);
};

}  // namespace codegen_llvm
