// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <vector>

#include "codegen/backend.h"
#include "codegen/target.h"
#include "fpag/base/result.h"
#include "pipeline/emit_mode.h"

namespace pipeline {

// Whether the compiled-in backend can write this kind of output for this
// target. False for a backend this build does not carry, and for a kind
// it has no writer for.
[[nodiscard]] bool backend_supports(codegen::Backend backend,
                                    codegen::OutputKind kind,
                                    const codegen::Target& target);

// Whether the backend can run the middle end. An implementation with no
// optimizer behind it answers false, so a release request is not sent to
// code that would refuse it later.
[[nodiscard]] bool backend_can_optimize(codegen::Backend backend);

// Whether the backend's output is the target's executable itself. Only a
// backend that writes a final module finishes a build with no linker;
// every other backend writes an object a linker still has to turn into
// one.
[[nodiscard]] bool backend_writes_final_module(codegen::Backend backend,
                                               const codegen::Target& target);

// The output kind a mode asks a backend for on this target: an
// executable is a final module where the target's executables are
// modules, and an object the linker turns into one everywhere else.
[[nodiscard]] codegen::OutputKind output_kind_for(
    EmitMode mode,
    const codegen::Target& target);

// The backend a command uses when it named none, for a request of this
// kind: the target's own preference when it can answer, otherwise the
// other implementation when it can. When neither can, the target's
// default stands so the pipeline's diagnostic names that backend rather
// than none.
[[nodiscard]] codegen::Backend select_backend(const codegen::Target& target,
                                              codegen::OutputKind kind,
                                              bool optimize);

// Runs the backend's emitter. The request is moved into the
// implementation, so the storage travels with it.
[[nodiscard]] base::Result<std::vector<u8>, codegen::EmitError>
emit_with_backend(codegen::Backend backend, codegen::EmitRequest request);

// The same, for a request that may produce more than one artifact: a backend
// that can split code generation answers with one object per part, and one
// that cannot answers the artifact `emit_with_backend` would have returned.
[[nodiscard]] base::Result<std::vector<std::vector<u8>>, codegen::EmitError>
emit_artifacts_with_backend(codegen::Backend backend,
                            codegen::EmitRequest request);

}  // namespace pipeline
