// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <vector>

#include "codegen/backend.h"
#include "codegen/target.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"

namespace codegen::x86_64 {

// The direct x86_64 backend: alcy's own emitter, which answers a Linux
// x86_64 target with a relocatable object the link step turns into a
// program. It exists so a debug build does not wait on LLVM, and it
// grows its lowering op by op rather than inheriting one.
struct X8664 {
  static constexpr codegen::Backend ID = codegen::Backend::DirectX86_64;

  static bool supports(codegen::OutputKind kind, const codegen::Target& target);

  static base::Result<std::vector<u8>, codegen::EmitError> emit(
      codegen::EmitRequest request);
};

}  // namespace codegen::x86_64
