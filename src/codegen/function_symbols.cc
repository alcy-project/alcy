// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "codegen/function_symbols.h"

#include <string>

#include "ir/common.h"
#include "ir/function.h"
#include "ir/storage.h"
#include "ir/type.h"
#include "symbol/mangle.h"

namespace codegen {

std::string linkable_name(const ir::Function& function,
                          const ir::Storage& storage,
                          const ir::SymbolTable& strings) {
  symbol::Signature signature;
  signature.path = std::string(strings.get(function.meta.path));
  signature.name = std::string(strings.get(function.meta.name));
  signature.owner = function.meta.owner;
  switch (function.meta.kind) {
    case ir::SymbolKind::Free:
      signature.kind = symbol::Signature::Kind::Free;
      break;
    case ir::SymbolKind::Assoc:
      signature.kind = symbol::Signature::Kind::Assoc;
      break;
    case ir::SymbolKind::Method:
      signature.kind = symbol::Signature::Kind::Method;
      break;
    default: break;
  }
  const ir::TypeIdxRange generics = function.meta.generics;
  signature.generics.reserve(generics.size());
  for (u32 i = 0; i < generics.size(); ++i) {
    signature.generics.emplace_back(generics.head().idx + i);
  }
  return symbol::mangle(signature, storage, strings);
}

bool is_entry_candidate(const ir::Storage& storage,
                        const ir::SymbolTable& strings,
                        const ir::Function& function) {
  if (function.meta.kind != ir::SymbolKind::Free) {
    return false;
  }
  if (strings.get(function.meta.name) != "main") {
    return false;
  }
  if (!function.meta.param_types.empty()) {
    return false;
  }
  const ir::TypeTag tag = storage.types()[function.meta.return_type.idx].tag;
  return tag == ir::TypeTag::Void || tag == ir::TypeTag::I32 ||
         tag == ir::TypeTag::Enum;
}

}  // namespace codegen
