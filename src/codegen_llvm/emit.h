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

  // The same request, written as one object per part. The partitions are a
  // function of the module and of nothing else, so two runs of one package
  // write the same objects whatever `-j` said, and they are code-generated
  // on as many threads as the request allows. A request for any other kind,
  // or for one part, answers the single artifact `emit` does.
  static base::Result<std::vector<std::vector<u8>>, codegen::EmitError>
  emit_artifacts(codegen::EmitRequest request);
};

}  // namespace codegen_llvm
