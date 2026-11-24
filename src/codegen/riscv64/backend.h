// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <vector>

#include "codegen/backend.h"
#include "codegen/target.h"

namespace codegen::riscv64 {

struct Riscv64 {
  static constexpr codegen::Backend ID = codegen::Backend::DirectRiscv64;

  [[nodiscard]] static bool supports(codegen::OutputKind kind,
                                     const codegen::Target& target);

  static base::Result<std::vector<u8>, codegen::EmitError> emit(
      codegen::EmitRequest request);
};

}  // namespace codegen::riscv64
