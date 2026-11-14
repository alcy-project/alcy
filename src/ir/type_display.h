// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#pragma once

#include <string>

#include "ir/common.h"
#include "ir/storage.h"
#include "ir/symbol_table.h"

namespace ir {

// Renders a decided type the way source would spell it: a primitive by
// its keyword, a struct or enum by its interned name, and a composite
// structurally. Diagnostics and the language server read a type through
// this one function, so they cannot disagree about its name.
std::string type_name(const StorageState& state,
                      const SymbolTable& strings,
                      TypeIdx type);

}  // namespace ir
