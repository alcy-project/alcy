// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "fpag/base/numeric.h"
#include "ir/common.h"
#include "ir/function.h"
#include "ir/symbol_table.h"

namespace ir {

// One row of a package's public surface (ADR-0078): the source name and
// what the symbol is. A row whose `function` is valid names the lowered
// code that carries its signature; a row with `needs_body` set names a
// declaration a dependent cannot resolve, because instantiating it needs
// the body only the package's own artifact holds.
struct SummaryEntry {
  FunctionIdx function;
  str::StringPoolId name = str::INVALID_STRING_POOL_ID;
  SymbolKind kind = SymbolKind::Foreign;
  bool needs_body = false;
};

}  // namespace ir
