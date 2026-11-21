// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "ast/ast.h"

#include "diag/span.h"

namespace ast {

diag::Span item_name_span(const AstArena& ast, const ItemNode& item) {
  using K = ItemKind;
  switch (item.kind) {
    case K::Struct: return item.payload.get<ItemStruct>().name.span;
    case K::Enum: return item.payload.get<ItemEnum>().name.span;
    case K::Fn: return item.payload.get<ItemFn>().name.span;
    case K::Intrinsic: return item.payload.get<ItemIntrinsic>().name.span;
    case K::Static: return item.payload.get<ItemStatic>().name.span;
    case K::Const: return item.payload.get<ItemConst>().name.span;
    case K::Spec: return item.payload.get<ItemSpec>().name.span;
    case K::Impl: return ast.types[item.payload.get<ItemImpl>().type].span;
    case K::Extern:
    case K::Use: return item.span;
  }
  return item.span;
}

diag::Span item_name_span(const AstArena& ast, ItemIdx item) {
  if (!item.is_valid()) {
    return {};
  }
  return item_name_span(ast, ast.items[item]);
}

}  // namespace ast
