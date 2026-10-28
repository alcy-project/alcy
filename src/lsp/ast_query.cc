// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/ast_query.h"

#include <optional>

#include "ast/ast.h"
#include "diag/span.h"
#include "fpag/base/numeric.h"
#include "pipeline/pipeline_context.h"
#include "source/source.h"

namespace lsp {

namespace {

bool span_contains(diag::Span span, usize offset) {
  return span.offset <= offset && offset < span.offset + span.length;
}

}  // namespace

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

std::optional<ast::ExprIdx> innermost_expr(
    const pipeline::PipelineContext& context,
    usize offset,
    source::FileId root) {
  std::optional<ast::ExprIdx> best;
  usize best_length = 0;
  context.ast.exprs.for_each_node([&](const ast::ExprIdx index) {
    const diag::Span span = context.ast.exprs[index].span;
    if (span.file != root || !span_contains(span, offset)) {
      return;
    }
    if (!best.has_value() || span.length < best_length) {
      best = index;
      best_length = span.length;
    }
  });
  return best;
}

std::optional<ast::TypeIdx> innermost_type(
    const pipeline::PipelineContext& context,
    usize offset,
    source::FileId root) {
  std::optional<ast::TypeIdx> best;
  usize best_length = 0;
  context.ast.types.for_each_node([&](const ast::TypeIdx index) {
    const diag::Span span = context.ast.types[index].span;
    if (span.file != root || !span_contains(span, offset)) {
      return;
    }
    if (!best.has_value() || span.length < best_length) {
      best = index;
      best_length = span.length;
    }
  });
  return best;
}

}  // namespace lsp
