// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "ast/ast.h"
#include "fpag/base/numeric.h"
#include "i18n/messages.h"
#include "linter/context.h"
#include "linter/rules/rules.h"
#include "linter/walk.h"

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

void check_block(LintContext& ctx, ast::BlockIdx block) {
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

void check_needless_return(LintContext& ctx, ast::ItemIdx item) {
  const ast::ItemNode& node = ctx.ast.items[item];
  if (node.kind != ast::ItemKind::Fn) {
    return;
  }
  const ast::ItemFn& fn = node.payload.get<ast::ItemFn>();
  if (!fn.body.is_valid()) {
    return;
  }
  const ast::Block& body = ctx.ast.blocks[fn.body];
  // A body either ends on a tail expression, which is how a value is
  // written without a return, or on a statement. A return in either
  // position is the one the reader does not need.
  ast::ExprIdx tail = body.value;
  if (!tail.is_valid() && !body.statements.empty()) {
    const ast::StmtNode& last = ctx.ast.stmts[body.statements.back()];
    if (last.kind == ast::StmtKind::Expr) {
      tail = last.payload.get<ast::StmtExpr>().value;
    }
  }
  if (!tail.is_valid()) {
    return;
  }
  const ast::ExprNode& ret = ctx.ast.exprs[tail];
  if (ret.kind == ast::ExprKind::Return) {
    ctx.report<i18n::Key::LintNeedlessReturn>(ret.span);
  }
}

}  // namespace

void UnreachableCode::run(LintContext& ctx) {
  for_each_block(ctx, &check_block);
}

void WhileTrue::run(LintContext& ctx) {
  for_each_expr(ctx, &check_while);
}

void EmptyLoop::run(LintContext& ctx) {
  for_each_expr(ctx, &check_loop);
}

void NeedlessReturn::run(LintContext& ctx) {
  for_each_item(ctx, &check_needless_return);
}

}  // namespace linter::rules
