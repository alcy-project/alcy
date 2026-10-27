// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/ast_query.h"

#include "ast/ast.h"
#include "diag/span.h"

namespace lsp {

diag::Span item_name_span(const ast::AstArena& ast, const ast::ItemNode& item) {
  using K = ast::ItemKind;
  switch (item.kind) {
    case K::Struct: return item.payload.get<ast::ItemStruct>().name.span;
    case K::Enum: return item.payload.get<ast::ItemEnum>().name.span;
    case K::Fn: return item.payload.get<ast::ItemFn>().name.span;
    case K::Intrinsic: return item.payload.get<ast::ItemIntrinsic>().name.span;
    case K::Static: return item.payload.get<ast::ItemStatic>().name.span;
    case K::Const: return item.payload.get<ast::ItemConst>().name.span;
    case K::Spec: return item.payload.get<ast::ItemSpec>().name.span;
    case K::Impl: return ast.types[item.payload.get<ast::ItemImpl>().type].span;
    case K::Extern:
    case K::Use: return item.span;
  }
  return item.span;
}

}  // namespace lsp
