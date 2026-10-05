// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <vector>

#include "codegen/backend.h"
#include "codegen/target.h"
#include "fpag/base/result.h"

namespace pipeline {

// Whether the compiled-in backend can write this kind of output for this
// target. False for a backend this build does not carry, and for a kind
// it has no writer for.
[[nodiscard]] bool backend_supports(codegen::Backend backend,
                                    codegen::OutputKind kind,
                                    const codegen::Target& target);

// Runs the backend's emitter. The request is moved into the
// implementation, so the storage travels with it.
[[nodiscard]] base::Result<std::vector<u8>, codegen::EmitError>
emit_with_backend(codegen::Backend backend, codegen::EmitRequest request);

}  // namespace pipeline
