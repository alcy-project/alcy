// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "fpag/base/numeric.h"
#include "ir/common.h"
#include "ir/function.h"
#include "ir/symbol_table.h"

namespace ir {

// What a summary row names. The first four are the symbol kinds a
// function row carries, so a function row's byte is the `SymbolKind` it
// is written as; a struct or enum row names a nominal, whose index stands
// in the row's `function` field.
enum class SummaryKind : u8 {
  Foreign = 0,
  Free = 1,
  Assoc = 2,
  Method = 3,
  Struct = 4,
  Enum = 5,
};

// One row of a package's public surface (ADR-0078): the source name, the
// module it belongs to, and what the symbol is. A function row whose
// `function` is valid names the lowered code that carries its signature;
// a nominal row's `function` is the type index, invalid when the
// declaration is generic. A row with `needs_body` set names a declaration
// a dependent cannot resolve, because instantiating it needs the body
// only the package's own artifact holds.
struct SummaryEntry {
  FunctionIdx function;
  str::StringPoolId name = str::INVALID_STRING_POOL_ID;
  // The module path, dotted from the package root; empty for the root.
  str::StringPoolId path = str::EMPTY_STRING_ID;
  // The symbol the item's code is written under, so a dependent calls
  // it by the name the library's own artifact exports.
  str::StringPoolId symbol = str::INVALID_STRING_POOL_ID;
  SummaryKind kind = SummaryKind::Free;
  bool needs_body = false;
};

}  // namespace ir
