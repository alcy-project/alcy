// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string>
#include <string_view>

#include "fpag/base/numeric.h"

namespace ir {
struct Function;
class Storage;
class SymbolTable;
}  // namespace ir

namespace codegen {

// The name a dependent links against for an exported function: the same
// mangling every backend gives the IR signature.
[[nodiscard]] std::string linkable_name(const ir::Function& function,
                                        const ir::Storage& storage,
                                        const ir::SymbolTable& strings);

// Whether this function is the program's entry: a free, argumentless
// `main`, the same test the analyzer applies.
[[nodiscard]] bool is_entry_candidate(const ir::Storage& storage,
                                      const ir::SymbolTable& strings,
                                      const ir::Function& function);

}  // namespace codegen
