// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "ast/ast.h"
#include "linter/context.h"

namespace linter {

// The walk's ledger: one callback per node family it reports. A caller
// fills the slots it wants; a null slot is not called. The schedule -
// the order, the child paths, the entry into nested bodies - is the
// walk's, so every rule reads the module the same way.
struct Visitor {
  void (*item)(LintContext&, ast::ItemIdx) = nullptr;
  void (*expr)(LintContext&, ast::ExprIdx) = nullptr;
  void (*stmt)(LintContext&, ast::StmtIdx) = nullptr;
  void (*block)(LintContext&, ast::BlockIdx) = nullptr;
  void (*pattern)(LintContext&, ast::PatternIdx) = nullptr;
  void (*type)(LintContext&, ast::TypeIdx) = nullptr;
  // Every path the module names outside its own use items: the value
  // paths, the type paths, the structurally named patterns and struct
  // literals, and a fixed array's length path.
  void (*path)(LintContext&, ast::PathIdx) = nullptr;
};

// Walks the module being checked: every item and the types its signature
// declares, every expression and statement in a body, the patterns, and
// the paths all of those name. Callbacks run in traversal order.
void walk_module(LintContext& ctx, const Visitor& visitor);

}  // namespace linter
