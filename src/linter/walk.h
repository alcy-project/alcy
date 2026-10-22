// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "ast/ast.h"
#include "linter/context.h"

namespace linter {

// The walk's whole surface: one call per node of the kind a rule
// checks, over the module being checked, in traversal order. The
// schedule - the order, the child paths, the entry into nested bodies -
// is the walk's, not the caller's, so every rule reads the module the
// same way and a rule states the node kind it wants and nothing else.
using ItemVisit = void (*)(LintContext&, ast::ItemIdx);
using ExprVisit = void (*)(LintContext&, ast::ExprIdx);
using BlockVisit = void (*)(LintContext&, ast::BlockIdx);

void for_each_item(LintContext& ctx, ItemVisit visit);
void for_each_expr(LintContext& ctx, ExprVisit visit);
void for_each_block(LintContext& ctx, BlockVisit visit);

}  // namespace linter
