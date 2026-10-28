// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string>

#include "ir/common.h"
#include "ir/storage.h"
#include "ir/symbol_table.h"

namespace lsp {

// Renders a decided type the way source would spell it: a primitive by
// its keyword, a struct or enum by its interned name, and a composite
// structurally. The hover and the inlay hints read the compiler's type
// through this one function, so they cannot disagree about its name.
std::string type_name(const ir::StorageState& state,
                      const ir::SymbolTable& strings,
                      ir::TypeIdx type);

}  // namespace lsp
