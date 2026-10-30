// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <optional>
#include <string>
#include <string_view>

#include "ast/ast.h"
#include "diag/span.h"
#include "fpag/base/numeric.h"
#include "i18n/messages.h"
#include "ir/common.h"
#include "linter/context.h"
#include "linter/rules/rules.h"
#include "linter/types.h"

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

// The source bytes a span covers, or nullopt when the source manager
// cannot resolve them.
std::optional<std::string_view> source_of_span(const LintContext& ctx,
                                               diag::Span span) {
  const std::optional<std::string_view> bytes = ctx.sources.bytes(span.file);
  if (!bytes.has_value()) {
    return std::nullopt;
  }
  const usize begin = span.offset;
  const usize end = begin + span.length;
  if (begin > bytes->size() || end > bytes->size() || end < begin) {
    return std::nullopt;
  }
  return bytes->substr(span.offset, span.length);
}

// The source bytes one expression covers.
std::optional<std::string_view> source_of(const LintContext& ctx,
                                          const ast::ExprNode& node) {
  return source_of_span(ctx, node.span);
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

// The bytes of a condition, wrapped in parentheses when joining it to
// another with `&&` would regroup it. `||` is the only operator looser
// than `&&`, so every other condition reads the same bare.
std::optional<std::string> condition_text(LintContext& ctx, ast::CondIdx cond) {
  if (!cond.is_valid()) {
    return std::nullopt;
  }
  const ast::Cond& node = ctx.ast.conds[cond];
  if (node.is_pattern || !node.value.is_valid()) {
    return std::nullopt;
  }
  const ast::ExprNode& value = ctx.ast.exprs[node.value];
  const std::optional<std::string_view> text = source_of(ctx, value);
  if (!text.has_value()) {
    return std::nullopt;
  }
  const bool regroup =
      value.kind == ast::ExprKind::Binary &&
      value.payload.get<ast::ExprBinary>().op == ast::BinaryOp::Or;
  std::string out;
  if (regroup) {
    out += '(';
  }
  out.append(*text);
  if (regroup) {
    out += ')';
  }
  return out;
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
  const u32 finding = ctx.report<i18n::Key::LintNestedIf>(inner_node.span);
  // The repair states the two tests as one: `if a { if b { body } }` is
  // `if a && b { body }`, which the outer span covers whole. An
  // `if let` tests a pattern rather than a value, so `condition_text`
  // answers nothing and the finding keeps its repairless form.
  if (!inner.then_block.is_valid()) {
    return;
  }
  const std::optional<std::string> outer_cond = condition_text(ctx, outer.cond);
  const std::optional<std::string> inner_cond = condition_text(ctx, inner.cond);
  if (!outer_cond.has_value() || !inner_cond.has_value()) {
    return;
  }
  const std::optional<std::string_view> body =
      source_of_span(ctx, ctx.ast.blocks[inner.then_block].span);
  if (!body.has_value()) {
    return;
  }
  std::string replacement = "if ";
  replacement += *outer_cond;
  replacement += " && ";
  replacement += *inner_cond;
  replacement += ' ';
  replacement.append(*body);
  ctx.fix(finding, node.span, replacement);
}

// The expression a block ends on: its tail value, or the single
// expression statement it holds. Invalid for a block of any other shape.
ast::ExprIdx block_end(LintContext& ctx, ast::BlockIdx block) {
  const ast::Block& node = ctx.ast.blocks[block];
  if (node.value.is_valid()) {
    return node.value;
  }
  if (node.statements.size() != 1) {
    return ast::ExprIdx::invalid();
  }
  const ast::StmtNode& stmt = ctx.ast.stmts[node.statements[0]];
  if (stmt.kind != ast::StmtKind::Expr) {
    return ast::ExprIdx::invalid();
  }
  return stmt.payload.get<ast::StmtExpr>().value;
}

// The boolean a block returns, or nullopt when its only outcome is not
// `ret true` or `ret false`.
std::optional<bool> returned_bool(LintContext& ctx, ast::BlockIdx block) {
  const ast::ExprIdx end = block_end(ctx, block);
  if (!end.is_valid()) {
    return std::nullopt;
  }
  const ast::ExprNode& node = ctx.ast.exprs[end];
  if (node.kind != ast::ExprKind::Return) {
    return std::nullopt;
  }
  const ast::ExprReturn& ret = node.payload.get<ast::ExprReturn>();
  if (!ret.value.is_valid()) {
    return std::nullopt;
  }
  const ast::ExprNode& value = ctx.ast.exprs[ret.value];
  if (value.kind != ast::ExprKind::Literal) {
    return std::nullopt;
  }
  const ast::Literal& literal =
      ctx.ast.literals[value.payload.get<ast::ExprLiteral>().value];
  if (literal.kind != ast::LiteralKind::Bool) {
    return std::nullopt;
  }
  return literal.spelling == "true";
}

// An if whose branches return the two boolean literals is the condition
// itself: `if c { ret true } else { ret false }` returns `c`, and the
// swapped pair returns its negation. The shape is the one the explicit
// `ret` gives it, not a tail value.
void check_needless_bool(LintContext& ctx, ast::ExprIdx expr) {
  const ast::ExprNode& node = ctx.ast.exprs[expr];
  if (node.kind != ast::ExprKind::If) {
    return;
  }
  const ast::ExprIf& branch = node.payload.get<ast::ExprIf>();
  if (!branch.then_block.is_valid() || !branch.else_block.is_valid()) {
    return;
  }
  // An `if let` tests a pattern, not a boolean value.
  const ast::Cond& cond = ctx.ast.conds[branch.cond];
  if (cond.is_pattern) {
    return;
  }
  const std::optional<bool> then_value = returned_bool(ctx, branch.then_block);
  const std::optional<bool> else_value = returned_bool(ctx, branch.else_block);
  if (!then_value.has_value() || !else_value.has_value()) {
    return;
  }
  // Both branches agreeing is a constant, not the condition.
  if (*then_value == *else_value) {
    return;
  }
  const std::optional<ir::TypeIdx> type = expression_type(ctx, cond.value);
  if (!type.has_value() || !is_bool_type(ctx, *type)) {
    return;
  }
  const u32 finding = ctx.report<i18n::Key::LintNeedlessBool>(node.span);
  // The repair keeps the condition the reader wrote. The swapped pair
  // inverts it, and the negation is parenthesized because a condition
  // is any expression.
  const std::optional<std::string_view> text =
      source_of(ctx, ctx.ast.exprs[cond.value]);
  if (!text.has_value()) {
    return;
  }
  std::string replacement = "ret ";
  if (*then_value) {
    replacement.append(*text);
  } else {
    replacement += "!(";
    replacement.append(*text);
    replacement += ')';
  }
  ctx.fix(finding, node.span, replacement);
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

void NeedlessBool::check_expr(LintContext& ctx, ast::ExprIdx expr) {
  check_needless_bool(ctx, expr);
}

}  // namespace linter::rules
