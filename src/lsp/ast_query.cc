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

std::optional<BindingType> binding_type_at(
    const pipeline::PipelineContext& context,
    usize offset,
    source::FileId root) {
  std::optional<BindingType> found;
  context.ast.stmts.for_each_node([&](const ast::StmtIdx index) {
    if (found.has_value()) {
      return;
    }
    const ast::StmtNode& statement = context.ast.stmts[index];
    if (statement.kind != ast::StmtKind::Decl || statement.span.file != root) {
      return;
    }
    const ast::StmtDecl& declaration = statement.payload.get<ast::StmtDecl>();
    if (declaration.pattern == ast::PatternIdx::invalid()) {
      return;
    }
    const ast::PatternNode& pattern = context.ast.patterns[declaration.pattern];
    diag::Span name;
    if (pattern.kind == ast::PatternKind::Ident) {
      name = pattern.payload.ident.name.span;
    } else if (pattern.kind == ast::PatternKind::MutIdent) {
      name = pattern.payload.mut_ident.name.span;
    } else {
      return;
    }
    if (name.file == root && span_contains(name, offset)) {
      diag::Span written;
      if (declaration.type != ast::TypeIdx::invalid()) {
        written = context.ast.types[declaration.type].span;
      }
      found = BindingType{written, declaration.init};
    }
  });
  return found;
}

}  // namespace lsp
