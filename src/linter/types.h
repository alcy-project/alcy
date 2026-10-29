// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <optional>

#include "ast/ast.h"
#include "fpag/base/numeric.h"
#include "ir/common.h"
#include "ir/type.h"
#include "linter/context.h"

namespace linter {

// The checker's recorded type for an expression in the module being
// checked. The uninstantiated entry is the plain answer; an expression
// the checker only reached through generic instantiations answers when
// every instantiation agrees and reports nothing when they do not, so a
// rule never acts on one arbitrary member of a generic family.
std::optional<ir::TypeIdx> expression_type(const LintContext& ctx,
                                           ast::ExprIdx expr);

// The type node a checked type names, or null for an index the package
// does not hold.
const ir::TypeNode* type_node(const LintContext& ctx, ir::TypeIdx type);

// Whether the checked type is one of the two floating-point types.
bool is_float_type(const LintContext& ctx, ir::TypeIdx type);

// Whether the checked type is bool.
bool is_bool_type(const LintContext& ctx, ir::TypeIdx type);

// Whether the checked type is a shared reference.
bool is_shared_reference(const LintContext& ctx, ir::TypeIdx type);

}  // namespace linter
