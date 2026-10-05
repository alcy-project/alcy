// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <span>
#include <vector>

#include "ir/common.h"
#include "ir/storage.h"

namespace codegen::wasm {

// The functions a program reaches from `roots`: the closure of the call
// graph, with an edge for every operand that names a function. An edge
// that is not a call keeps a function value's body alive too, so pruning
// never drops code the program still names.
[[nodiscard]] std::vector<bool> reachable_functions(
    const ir::Storage& storage,
    std::span<const ir::FunctionIdx> roots);

}  // namespace codegen::wasm
