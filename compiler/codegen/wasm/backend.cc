// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "codegen/wasm/backend.h"

#include <utility>
#include <vector>

#include "codegen/backend.h"
#include "codegen/target.h"
#include "codegen/wasm/emit.h"

namespace codegen::wasm {

bool Wasm::supports(codegen::OutputKind kind, const codegen::Target& target) {
  return kind == codegen::OutputKind::Module && target.is_wasm();
}

base::Result<std::vector<u8>, codegen::EmitError> Wasm::emit(
    codegen::EmitRequest request) {
  if (!supports(request.kind, request.target)) {
    return base::make_err(codegen::EmitError::Unsupported);
  }
  // No optimizer lives behind this emitter, and unoptimized machine code
  // under `--release` would be a silent lie about what was built.
  if (request.optimize) {
    return base::make_err(codegen::EmitError::NoOptimizer);
  }
  return emit_module(std::move(request));
}

}  // namespace codegen::wasm
