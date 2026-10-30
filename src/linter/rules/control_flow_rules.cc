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

// Whether an expression transfers control away from the statement that
// contains it, so the statement after it can never run. A loop without
// a visible break is not treated as diverging: the body may break out,
// and the analysis that would prove it does not is the borrow checker's
// rather than a lint's.
bool expr_diverges(const LintContext& ctx, ast::ExprIdx expr);
bool block_diverges(const LintContext& ctx, ast::BlockIdx block);

bool stmt_diverges(const LintContext& ctx, ast::StmtIdx stmt) {
  const ast::StmtNode& node = ctx.ast.stmts[stmt];
  if (node.kind != ast::StmtKind::Expr) {
    return false;
  }
  return expr_diverges(ctx, node.payload.get<ast::StmtExpr>().value);
}

bool block_diverges(const LintContext& ctx, ast::BlockIdx block) {
  if (!block.is_valid()) {
    return false;
  }
  const ast::Block& node = ctx.ast.blocks[block];
  if (node.value.is_valid()) {
    return expr_diverges(ctx, node.value);
  }
  if (node.statements.empty()) {
    return false;
  }
  return stmt_diverges(ctx, node.statements.back());
}

bool expr_diverges(const LintContext& ctx, ast::ExprIdx expr) {
  if (!expr.is_valid()) {
    return false;
  }
  const ast::ExprNode& node = ctx.ast.exprs[expr];
  using K = ast::ExprKind;
  switch (node.kind) {
    case K::Return:
    case K::Break:
    case K::Continue: return true;
    case K::If: {
      const ast::ExprIf& e = node.payload.get<ast::ExprIf>();
      return e.else_block.is_valid() && block_diverges(ctx, e.then_block) &&
             block_diverges(ctx, e.else_block);
    }
    case K::Match: {
      const ast::ExprMatch& e = node.payload.get<ast::ExprMatch>();
      if (e.arms.empty()) {
        return false;
      }
      for (const ast::ExprMatchArm& arm : e.arms) {
        if (!expr_diverges(ctx, arm.body)) {
          return false;
        }
      }
      return true;
    }
    case K::Block:
      return block_diverges(ctx, node.payload.get<ast::ExprBlock>().block);
    default: return false;
  }
}

void check_unreachable(LintContext& ctx, ast::BlockIdx block) {
  const ast::Block& node = ctx.ast.blocks[block];
  bool diverged = false;
  for (const ast::StmtIdx stmt : node.statements) {
    if (diverged) {
      ctx.report<i18n::Key::LintUnreachableCode>(ctx.ast.stmts[stmt].span);
      break;
    }
    diverged = stmt_diverges(ctx, stmt);
  }
  if (diverged && node.value.is_valid()) {
    ctx.report<i18n::Key::LintUnreachableCode>(ctx.ast.exprs[node.value].span);
  }
}

void check_while(LintContext& ctx, ast::ExprIdx expr) {
  const ast::ExprNode& node = ctx.ast.exprs[expr];
  if (node.kind != ast::ExprKind::While) {
    return;
  }
  const ast::ExprWhile& loop = node.payload.get<ast::ExprWhile>();
  if (!loop.cond.is_valid()) {
    return;
  }
  const ast::Cond& cond = ctx.ast.conds[loop.cond];
  if (cond.is_pattern || !cond.value.is_valid()) {
    return;
  }
  const ast::ExprNode& value = ctx.ast.exprs[cond.value];
  if (value.kind != ast::ExprKind::Literal) {
    return;
  }
  const ast::Literal& literal =
      ctx.ast.literals[value.payload.get<ast::ExprLiteral>().value];
  if (literal.kind != ast::LiteralKind::Bool || literal.spelling != "true") {
    return;
  }
  const u32 finding = ctx.report<i18n::Key::LintWhileTrue>(node.span);
  ctx.label<i18n::Key::LintWhileTrueHelp>(finding, node.span);
  // The repair is sure only when the text the swap covers is exactly
  // the keyword and the condition: a spread or parenthesized `true`
  // would leave bytes the swap does not own behind (ADR-0111 - a repair
  // is the rule's assertion that it knows the bytes it replaces).
  const std::optional<std::string_view> bytes =
      ctx.sources.bytes(node.span.file);
  if (!bytes.has_value()) {
    return;
  }
  const std::string_view text = *bytes;
  const usize begin = node.span.offset;
  const usize end = value.span.offset + value.span.length;
  if (begin > text.size() || end > text.size() || end < begin ||
      text.substr(begin, end - begin) != "while true") {
    return;
  }
  ctx.fix(finding,
          diag::Span{node.span.file, static_cast<u32>(begin),
                     static_cast<u32>(end - begin)},
          "loop");
}

void check_loop(LintContext& ctx, ast::ExprIdx expr) {
  const ast::ExprNode& node = ctx.ast.exprs[expr];
  if (node.kind != ast::ExprKind::Loop) {
    return;
  }
  const ast::Block& body =
      ctx.ast.blocks[node.payload.get<ast::ExprLoop>().body];
  if (body.statements.empty() && !body.value.is_valid()) {
    ctx.report<i18n::Key::LintEmptyLoop>(node.span);
  }
}

}  // namespace

// Whether a pattern matches a value of any shape, so an arm carrying
// it and no guard makes every arm below it unreachable.
//
// Only three kinds answer: a wildcard, and the two bindings the parser
// makes of a lone `_`-or-lowercase name. Every uppercase or qualified
// name is a unit variant rather than a binding, and a reference,
// tuple, or struct pattern destructures, so none of them covers
// everything. That is the same set the checker's exhaustiveness treats
// as an anything-pattern, so the rule never calls an arm dead that the
// compiler thinks is reachable.
bool pattern_is_irrefutable(const LintContext& ctx, ast::PatternIdx pattern) {
  if (!pattern.is_valid()) {
    return false;
  }
  const ast::PatternKind kind = ctx.ast.patterns[pattern].kind;
  return kind == ast::PatternKind::Wildcard ||
         kind == ast::PatternKind::Ident || kind == ast::PatternKind::MutIdent;
}

// The arms a match can never reach: everything below the first arm that
// matches anything. A guard tests after its pattern binds, so a guarded
// arm covers nothing and the arm below it is still reachable. The
// checker already rejects a repeated literal, which is the case a
// pattern alone answers; what it does not see is an arm hidden behind
// an anything-pattern.
void check_unreachable_pattern(LintContext& ctx, ast::ExprIdx expr) {
  const ast::ExprNode& node = ctx.ast.exprs[expr];
  if (node.kind != ast::ExprKind::Match) {
    return;
  }
  const ast::ExprMatch& match = node.payload.get<ast::ExprMatch>();
  ast::PatternIdx covering = ast::PatternIdx::invalid();
  for (const ast::ExprMatchArm& arm : match.arms) {
    if (covering.is_valid()) {
      const ast::PatternNode& pattern = ctx.ast.patterns[arm.pattern];
      const u32 finding =
          ctx.report<i18n::Key::LintUnreachablePattern>(pattern.span);
      ctx.label<i18n::Key::LintUnreachablePatternNote>(
          finding, ctx.ast.patterns[covering].span);
      continue;
    }
    if (arm.guard.is_valid()) {
      continue;
    }
    if (pattern_is_irrefutable(ctx, arm.pattern)) {
      covering = arm.pattern;
    }
  }
}

void UnreachableCode::check_block(LintContext& ctx, ast::BlockIdx block) {
  check_unreachable(ctx, block);
}

void UnreachablePattern::check_expr(LintContext& ctx, ast::ExprIdx expr) {
  check_unreachable_pattern(ctx, expr);
}

void WhileTrue::check_expr(LintContext& ctx, ast::ExprIdx expr) {
  check_while(ctx, expr);
}

void EmptyLoop::check_expr(LintContext& ctx, ast::ExprIdx expr) {
  check_loop(ctx, expr);
}

}  // namespace linter::rules
