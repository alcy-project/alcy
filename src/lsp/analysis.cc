// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/analysis.h"

#include <optional>

#include "analyzer/resolve.h"
#include "analyzer/types.h"
#include "ast/ast.h"
#include "diag/bag.h"
#include "fpag/base/result.h"
#include "ir/common.h"
#include "lsp/document.h"
#include "pipeline/frontend.h"
#include "pipeline/pipeline_context.h"
#include "pipeline/std_select.h"
#include "source/source.h"

namespace lsp {

base::Result<analyzer::CheckedPackage, diag::Reported> analyze_document(
    pipeline::PipelineContext& context,
    const Document& document,
    source::FileId* root) {
  const source::FileId file =
      context.sources.add_virtual(document.uri(), document.text());
  if (root != nullptr) {
    *root = file;
  }
  base::Result<analyzer::ModuleTree, diag::Reported> tree =
      pipeline::front_end_root(context, file, pipeline::full_std_selection());
  if (tree.is_err()) {
    return base::make_err(diag::Reported{});
  }
  return pipeline::analyze_tree(context, tree.value());
}

std::optional<ir::TypeIdx> checked_expr_type(
    const analyzer::CheckedPackage& package,
    ast::ExprIdx expr) {
  std::optional<ir::TypeIdx> fallback;
  for (const analyzer::CheckedModule& module : package.modules) {
    for (const analyzer::CheckedModule::ExprType& entry : module.expr_types) {
      if (entry.expr != expr) {
        continue;
      }
      if (entry.inst == analyzer::NO_INST) {
        return entry.type;
      }
      if (!fallback.has_value()) {
        fallback = entry.type;
      }
    }
  }
  return fallback;
}

}  // namespace lsp
