// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "linter/types.h"

#include <optional>

#include "analyzer/types.h"
#include "ast/ast.h"
#include "fpag/base/numeric.h"
#include "ir/common.h"
#include "ir/storage.h"
#include "ir/type.h"
#include "linter/context.h"

namespace linter {

std::optional<ir::TypeIdx> expression_type(const LintContext& ctx,
                                           ast::ExprIdx expr) {
  if (!expr.is_valid() || ctx.module >= ctx.package.modules.size()) {
    return std::nullopt;
  }
  const analyzer::CheckedModule& module = ctx.package.modules[ctx.module];
  if (const analyzer::CheckedModule::ExprType* entry =
          module.expr_type_of(expr, analyzer::NO_INST)) {
    return entry->type;
  }
  // A body checked only under instantiations keeps one entry per
  // instantiation. An answer every entry agrees on is safe to use; two
  // different answers are not, and the expression goes untyped.
  std::optional<ir::TypeIdx> answer;
  for (const analyzer::CheckedModule::ExprType& entry : module.expr_types) {
    if (entry.expr != expr) {
      continue;
    }
    if (!answer.has_value()) {
      answer = entry.type;
      continue;
    }
    if (*answer != entry.type) {
      return std::nullopt;
    }
  }
  return answer;
}

const ir::TypeNode* type_node(const LintContext& ctx, ir::TypeIdx type) {
  if (!type.is_valid()) {
    return nullptr;
  }
  const ir::StorageState& state = ctx.package.types->state();
  if (type.idx >= state.types.size()) {
    return nullptr;
  }
  return &state.types[type];
}

bool is_float_type(const LintContext& ctx, ir::TypeIdx type) {
  const ir::TypeNode* node = type_node(ctx, type);
  if (node == nullptr) {
    return false;
  }
  return node->tag == ir::TypeTag::F32 || node->tag == ir::TypeTag::F64;
}

bool is_shared_reference(const LintContext& ctx, ir::TypeIdx type) {
  const ir::TypeNode* node = type_node(ctx, type);
  return node != nullptr && node->tag == ir::TypeTag::Ref;
}

}  // namespace linter
