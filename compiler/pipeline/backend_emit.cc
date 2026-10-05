// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pipeline/backend_emit.h"

#include <utility>
#include <vector>

#include "codegen/backend.h"
#include "codegen/target.h"
#include "fpag/base/result.h"

// The dispatch names every implementation the build carries and nothing
// else. Keeping the conditionals here is what lets the rest of the
// pipeline be written against `codegen::Backend` alone: a build that
// carries no LLVM never includes its headers, and a build with no
// backend compiles the cases away rather than linking a stub.
#if ALCY_BACKEND_LLVM
#include "codegen_llvm/emit.h"
#endif
#if ALCY_BACKEND_DIRECT_WASM
#include "codegen/wasm/backend.h"
#endif

namespace pipeline {

bool backend_supports(codegen::Backend backend,
                      codegen::OutputKind kind,
                      const codegen::Target& target) {
  // A build with no implementation leaves these unused; the only answer
  // such a build has is false, below.
  (void)kind;
  (void)target;
  switch (backend) {
    case codegen::Backend::None: return false;
    case codegen::Backend::Llvm:
#if ALCY_BACKEND_LLVM
      return codegen_llvm::Llvm::supports(kind, target);
#else
      return false;
#endif
    case codegen::Backend::DirectWasm:
#if ALCY_BACKEND_DIRECT_WASM
      return codegen::wasm::Wasm::supports(kind, target);
#else
      return false;
#endif
  }
  return false;
}

base::Result<std::vector<u8>, codegen::EmitError> emit_with_backend(
    codegen::Backend backend,
    codegen::EmitRequest request) {
  // A build with no implementation never reaches the storage; the
  // request is dropped here rather than left unused.
  (void)request;
  switch (backend) {
    case codegen::Backend::None:
      return base::make_err(codegen::EmitError::Unsupported);
    case codegen::Backend::Llvm:
#if ALCY_BACKEND_LLVM
      return codegen::emit_with<codegen_llvm::Llvm>(std::move(request));
#else
      return base::make_err(codegen::EmitError::Unsupported);
#endif
    case codegen::Backend::DirectWasm:
#if ALCY_BACKEND_DIRECT_WASM
      return codegen::emit_with<codegen::wasm::Wasm>(std::move(request));
#else
      return base::make_err(codegen::EmitError::Unsupported);
#endif
  }
  return base::make_err(codegen::EmitError::Unsupported);
}

}  // namespace pipeline
