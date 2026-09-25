// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "fpag/str/string_pool_id.h"
#include "ir/common.h"

namespace ir {

// What a symbol names, so the linker-visible name can be derived from
// the signature rather than from the source name.
enum class SymbolKind : u8 {
  // Not mangled: a C entry point, or the synthesized program entry.
  Foreign,
  // A free function.
  Free,
  // An associated function in an impl block, with no receiver.
  Assoc,
  // An inherent method, which takes a receiver.
  Method,
};

struct FunctionMeta {
  TypeIdx return_type;
  TypeIdxRange param_types;

  // Source name, kept for diagnostics and for finding the entry point.
  str::StringPoolId name;
  // Dotted module path, empty for the root module.
  str::StringPoolId path = str::kEmptyStringId;
  SymbolKind kind = SymbolKind::Foreign;
  // Type arguments of the instantiation, empty for a non-generic item.
  TypeIdxRange generics;
};

struct Function {
  FunctionMeta meta;

  BlockIdxRange blocks;
};

}  // namespace ir
