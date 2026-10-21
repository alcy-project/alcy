// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <vector>

#include "codegen/backend.h"
#include "codegen/target.h"
#include "fpag/base/result.h"

namespace codegen::wasm {

// The direct wasm backend: alcy's own emitter, which answers a wasm
// target with a final module and needs no linker behind it.
struct Wasm {
  static constexpr codegen::Backend ID = codegen::Backend::DirectWasm;

  // A final module for a wasm machine, and nothing else: an object, IR,
  // or bitcode request belongs to a backend that has those forms.
  static bool supports(codegen::OutputKind kind, const codegen::Target& target);

  static base::Result<std::vector<u8>, codegen::EmitError> emit(
      codegen::EmitRequest request);
};

}  // namespace codegen::wasm
