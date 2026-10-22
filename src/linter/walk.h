// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <span>

#include "ast/ast.h"
#include "fpag/base/numeric.h"
#include "linter/context.h"

namespace linter {

// One traversal of the syntax tree, with a callback at each node a rule
// may care about. Function pointers and a void* rather than a visitor
// class: the linter has no virtual functions, and a rule that needs
// state keeps it in a struct the pointer names.
//
// Pre and exit callbacks bracket the children, which is what a rule
// that tracks scope needs: it pushes on the way in and pops on the way
// out. A callback left null is not called.
struct TreeVisitor {
  void* self = nullptr;
  void (*item)(void*, LintContext&, ast::ItemIdx) = nullptr;
  void (*item_exit)(void*, LintContext&, ast::ItemIdx) = nullptr;
  void (*expr)(void*, LintContext&, ast::ExprIdx) = nullptr;
  void (*expr_exit)(void*, LintContext&, ast::ExprIdx) = nullptr;
  void (*stmt)(void*, LintContext&, ast::StmtIdx) = nullptr;
  void (*stmt_exit)(void*, LintContext&, ast::StmtIdx) = nullptr;
  void (*block)(void*, LintContext&, ast::BlockIdx) = nullptr;
  void (*block_exit)(void*, LintContext&, ast::BlockIdx) = nullptr;
  void (*pattern)(void*, LintContext&, ast::PatternIdx) = nullptr;
  void (*type)(void*, LintContext&, ast::TypeIdx) = nullptr;
};

void walk_module(LintContext& ctx, u32 module, const TreeVisitor& visitor);
void walk_items(LintContext& ctx,
                std::span<const ast::ItemIdx> items,
                const TreeVisitor& visitor);
void walk_item(LintContext& ctx, ast::ItemIdx item, const TreeVisitor& visitor);
void walk_expr(LintContext& ctx, ast::ExprIdx expr, const TreeVisitor& visitor);
void walk_exprs(LintContext& ctx,
                std::span<const ast::ExprIdx> exprs,
                const TreeVisitor& visitor);
void walk_stmt(LintContext& ctx, ast::StmtIdx stmt, const TreeVisitor& visitor);
void walk_block(LintContext& ctx,
                ast::BlockIdx block,
                const TreeVisitor& visitor);
void walk_cond(LintContext& ctx, ast::CondIdx cond, const TreeVisitor& visitor);
void walk_pattern(LintContext& ctx,
                  ast::PatternIdx pattern,
                  const TreeVisitor& visitor);
void walk_type(LintContext& ctx, ast::TypeIdx type, const TreeVisitor& visitor);

}  // namespace linter
