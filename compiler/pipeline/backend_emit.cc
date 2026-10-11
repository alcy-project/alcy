// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pipeline/backend_emit.h"

#include <utility>
#include <vector>

#include "codegen/backend.h"
#include "codegen/target.h"
#include "fpag/base/result.h"
#include "pipeline/emit_mode.h"

// The dispatch names every implementation the build carries and nothing
// else. Keeping the conditionals here is what lets the rest of the
// pipeline be written against `codegen::Backend` alone: a build that
// carries no LLVM never includes its headers, and a build with no
// backend compiles the cases away rather than linking a stub.
#if ALCY_BACKEND_LLVM
#include "codegen_llvm/emit.h"
#endif
#include "codegen/wasm/backend.h"

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
      return codegen::wasm::Wasm::supports(kind, target);
  }
  return false;
}

bool backend_can_optimize(codegen::Backend backend) {
  switch (backend) {
    case codegen::Backend::None: return false;
    case codegen::Backend::Llvm:
#if ALCY_BACKEND_LLVM
      return true;
#else
      return false;
#endif
    case codegen::Backend::DirectWasm:
      // The direct emitter has no middle end of its own; the
      // pipeline's IR passes are the optimizer, and they run before
      // either backend is chosen.
      return true;
  }
  return false;
}

bool backend_writes_final_module(codegen::Backend backend,
                                 const codegen::Target& target) {
  return backend_supports(backend, codegen::OutputKind::Module, target);
}

codegen::OutputKind output_kind_for(EmitMode mode,
                                    const codegen::Target& target) {
  switch (mode) {
    case EmitMode::Executable:
      return target.is_wasm() ? codegen::OutputKind::Module
                              : codegen::OutputKind::Object;
    case EmitMode::Object: return codegen::OutputKind::Object;
    case EmitMode::LlvmIr: return codegen::OutputKind::Text;
    case EmitMode::LlvmBitcode: return codegen::OutputKind::Bitcode;
    case EmitMode::Ir:
    case EmitMode::IrBinary:
      // The pipeline writes these itself and never consults a backend;
      // the answer only keeps the selection total.
      return codegen::OutputKind::Object;
  }
  return codegen::OutputKind::Object;
}

codegen::Backend select_backend(const codegen::Target& target,
                                codegen::OutputKind kind,
                                bool optimize) {
  const codegen::Backend preferred = codegen::default_backend(target);
  const auto usable = [&](codegen::Backend backend) {
    if (!codegen::backend_available(backend)) {
      return false;
    }
    if (!backend_supports(backend, kind, target)) {
      return false;
    }
    return !optimize || backend_can_optimize(backend);
  };
  if (usable(preferred)) {
    return preferred;
  }
  for (const codegen::Backend candidate :
       {codegen::Backend::Llvm, codegen::Backend::DirectWasm}) {
    if (candidate != preferred && usable(candidate)) {
      return candidate;
    }
  }
  return preferred;
}

base::Result<std::vector<std::vector<u8>>, codegen::EmitError>
emit_artifacts_with_backend(codegen::Backend backend,
                            codegen::EmitRequest request) {
  (void)request;
  switch (backend) {
    case codegen::Backend::None:
      return base::make_err(codegen::EmitError::Unsupported);
    case codegen::Backend::Llvm:
#if ALCY_BACKEND_LLVM
      return codegen::emit_artifacts_with<codegen_llvm::Llvm>(
          std::move(request));
#else
      return base::make_err(codegen::EmitError::Unsupported);
#endif
    case codegen::Backend::DirectWasm:
      return codegen::emit_artifacts_with<codegen::wasm::Wasm>(
          std::move(request));
  }
  return base::make_err(codegen::EmitError::Unsupported);
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
      return codegen::emit_with<codegen::wasm::Wasm>(std::move(request));
  }
  return base::make_err(codegen::EmitError::Unsupported);
}

}  // namespace pipeline
