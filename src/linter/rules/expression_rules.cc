// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <optional>
#include <string_view>

#include "ast/ast.h"
#include "fpag/base/numeric.h"
#include "i18n/messages.h"
#include "linter/context.h"
#include "linter/rules/rules.h"

namespace linter::rules {

namespace {

// Whether two expressions are the same shape over the same names. The
// comparison is deliberately narrow: it answers yes only for the forms
// it fully understands and no for everything else, so a rule built on
// it never claims two different expressions are one. File-local, unlike
// a semantic equality, which would need the types the analyzer holds.
bool same_expr(const LintContext& ctx, ast::ExprIdx a, ast::ExprIdx b) {
  if (a == b) {
    return a.is_valid();
  }
  if (!a.is_valid() || !b.is_valid()) {
    return false;
  }
  const ast::ExprNode& left = ctx.ast.exprs[a];
  const ast::ExprNode& right = ctx.ast.exprs[b];
  if (left.kind != right.kind) {
    return false;
  }
  using K = ast::ExprKind;
  switch (left.kind) {
    case K::Literal: {
      const ast::Literal& l =
          ctx.ast.literals[left.payload.get<ast::ExprLiteral>().value];
      const ast::Literal& r =
          ctx.ast.literals[right.payload.get<ast::ExprLiteral>().value];
      return l.kind == r.kind && l.spelling == r.spelling &&
             l.is_negative == r.is_negative;
    }
    case K::Path: {
      const ast::Path& l = ctx.ast.paths[left.payload.get<ast::ExprPath>().idx];
      const ast::Path& r =
          ctx.ast.paths[right.payload.get<ast::ExprPath>().idx];
      if (l.segments.size() != r.segments.size()) {
        return false;
      }
      for (usize i = 0; i < l.segments.size(); ++i) {
        if (l.segments[i].name != r.segments[i].name) {
          return false;
        }
      }
      return true;
    }
    case K::Field: {
      const ast::ExprField& l = left.payload.get<ast::ExprField>();
      const ast::ExprField& r = right.payload.get<ast::ExprField>();
      return l.name.name == r.name.name &&
             same_expr(ctx, l.receiver, r.receiver);
    }
    case K::Index: {
      const ast::ExprIndex& l = left.payload.get<ast::ExprIndex>();
      const ast::ExprIndex& r = right.payload.get<ast::ExprIndex>();
      return same_expr(ctx, l.receiver, r.receiver) &&
             same_expr(ctx, l.index, r.index);
    }
    case K::Unary: {
      const ast::ExprUnary& l = left.payload.get<ast::ExprUnary>();
      const ast::ExprUnary& r = right.payload.get<ast::ExprUnary>();
      return l.op == r.op && same_expr(ctx, l.inner, r.inner);
    }
    case K::Binary: {
      const ast::ExprBinary& l = left.payload.get<ast::ExprBinary>();
      const ast::ExprBinary& r = right.payload.get<ast::ExprBinary>();
      return l.op == r.op && same_expr(ctx, l.lhs, r.lhs) &&
             same_expr(ctx, l.rhs, r.rhs);
    }
    case K::Tuple: {
      const ast::ExprTuple& l = left.payload.get<ast::ExprTuple>();
      const ast::ExprTuple& r = right.payload.get<ast::ExprTuple>();
      if (l.elements.size() != r.elements.size()) {
        return false;
      }
      for (usize i = 0; i < l.elements.size(); ++i) {
        if (!same_expr(ctx, l.elements[i], r.elements[i])) {
          return false;
        }
      }
      return true;
    }
    default: return false;
  }
}

bool is_comparison(ast::BinaryOp op) {
  using B = ast::BinaryOp;
  switch (op) {
    case B::Eq:
    case B::NotEq:
    case B::Gt:
    case B::Lt:
    case B::GtEq:
    case B::LtEq: return true;
    default: return false;
  }
}

// The source bytes one expression covers, or nullopt when the source
// manager cannot resolve them.
std::optional<std::string_view> source_of(const LintContext& ctx,
                                          const ast::ExprNode& node) {
  const std::optional<std::string_view> bytes =
      ctx.sources.bytes(node.span.file);
  if (!bytes.has_value()) {
    return std::nullopt;
  }
  const usize begin = node.span.offset;
  const usize end = begin + node.span.length;
  if (begin > bytes->size() || end > bytes->size() || end < begin) {
    return std::nullopt;
  }
  return bytes->substr(node.span.offset, node.span.length);
}

// Whether moving an expression out of the construct that wraps it
// cannot change how its neighbours group it.
bool binds_tightly(ast::ExprKind kind) {
  using K = ast::ExprKind;
  switch (kind) {
    case K::Literal:
    case K::Path:
    case K::Struct:
    case K::Tuple:
    case K::Array:
    case K::Call:
    case K::MethodCall:
    case K::Field:
    case K::Index:
    case K::Unary:
    case K::Borrow:
    case K::Deref:
    case K::Question: return true;
    default: return false;
  }
}

void check_double_negation(LintContext& ctx, ast::ExprIdx expr) {
  const ast::ExprNode& node = ctx.ast.exprs[expr];
  if (node.kind != ast::ExprKind::Unary) {
    return;
  }
  const ast::ExprUnary& outer = node.payload.get<ast::ExprUnary>();
  if (!outer.inner.is_valid()) {
    return;
  }
  const ast::ExprNode& inner = ctx.ast.exprs[outer.inner];
  if (inner.kind != ast::ExprKind::Unary) {
    return;
  }
  const ast::ExprUnary& nested = inner.payload.get<ast::ExprUnary>();
  if (nested.op != outer.op) {
    return;
  }
  const u32 finding = ctx.report<i18n::Key::LintDoubleNegation>(node.span);
  // The repair replaces both operators with the expression they wrap,
  // when moving it out cannot change how its neighbours group it. The
  // bytes are the inner expression exactly as the reader wrote them.
  if (!nested.inner.is_valid()) {
    return;
  }
  const ast::ExprNode& core = ctx.ast.exprs[nested.inner];
  if (!binds_tightly(core.kind)) {
    return;
  }
  const std::optional<std::string_view> text = source_of(ctx, core);
  if (text.has_value()) {
    ctx.fix(finding, node.span, *text);
  }
}

void check_self_comparison(LintContext& ctx, ast::ExprIdx expr) {
  const ast::ExprNode& node = ctx.ast.exprs[expr];
  if (node.kind != ast::ExprKind::Binary) {
    return;
  }
  const ast::ExprBinary& binary = node.payload.get<ast::ExprBinary>();
  if (!is_comparison(binary.op)) {
    return;
  }
  if (same_expr(ctx, binary.lhs, binary.rhs)) {
    ctx.report<i18n::Key::LintSelfComparison>(node.span);
  }
}

void check_nested_if(LintContext& ctx, ast::ExprIdx expr) {
  const ast::ExprNode& node = ctx.ast.exprs[expr];
  if (node.kind != ast::ExprKind::If) {
    return;
  }
  const ast::ExprIf& outer = node.payload.get<ast::ExprIf>();
  if (outer.else_block.is_valid() || !outer.then_block.is_valid()) {
    return;
  }
  const ast::Block& then_block = ctx.ast.blocks[outer.then_block];
  // The inner if may be the block's only statement or its tail value;
  // the two are the same shape with or without the separator.
  ast::ExprIdx inner_expr = then_block.value;
  if (!inner_expr.is_valid()) {
    if (then_block.statements.size() != 1) {
      return;
    }
    const ast::StmtNode& stmt = ctx.ast.stmts[then_block.statements[0]];
    if (stmt.kind != ast::StmtKind::Expr) {
      return;
    }
    inner_expr = stmt.payload.get<ast::StmtExpr>().value;
  }
  if (!inner_expr.is_valid()) {
    return;
  }
  const ast::ExprNode& inner_node = ctx.ast.exprs[inner_expr];
  if (inner_node.kind != ast::ExprKind::If) {
    return;
  }
  const ast::ExprIf& inner = inner_node.payload.get<ast::ExprIf>();
  if (inner.else_block.is_valid()) {
    return;
  }
  ctx.report<i18n::Key::LintNestedIf>(inner_node.span);
}

}  // namespace

void DoubleNegation::check_expr(LintContext& ctx, ast::ExprIdx expr) {
  check_double_negation(ctx, expr);
}

void SelfComparison::check_expr(LintContext& ctx, ast::ExprIdx expr) {
  check_self_comparison(ctx, expr);
}

void NestedIf::check_expr(LintContext& ctx, ast::ExprIdx expr) {
  check_nested_if(ctx, expr);
}

}  // namespace linter::rules
