// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pipeline/frontend.h"

#include <span>
#include <utility>

#include "analyzer/resolve.h"
#include "analyzer/types.h"
#include "borrow/borrow.h"
#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "fpag/base/result.h"
#include "lower/lower.h"
#include "pipeline/std_stage.h"
#include "pipeline/target.h"

namespace pipeline {

base::Result<FrontendOutput, diag::Reported> run_frontend(
    PipelineContext& ctx,
    analyzer::ModuleTree tree) {
  base::Result<analyzer::CheckedPackage, diag::Reported> checked =
      analyzer::check_package(tree, TARGET_WIDTH, ctx.ast, ctx.bag);
  if (checked.is_err() || ctx.bag.has_errors()) {
    return base::make_err(diag::Reported{});
  }
  analyzer::CheckedPackage package = std::move(checked).unwrap();
  // Staged prelude modules check with the package but read as
  // toolchain sources, so reported counts exclude them.
  const usize module_count =
      package.modules.size() > tree.staged_modules
          ? package.modules.size() - tree.staged_modules
          : 0;
  base::Result<lower::LoweredPackage, diag::Reported> lowered =
      lower::lower_package(std::move(package), TARGET_WIDTH, ctx.ast,
                           ctx.strings, ctx.bag);
  if (lowered.is_err() || ctx.bag.has_errors()) {
    return base::make_err(diag::Reported{});
  }
  lower::LoweredPackage package_ir = std::move(lowered).unwrap();
  const usize function_count =
      package_ir.storage->functions().size() > package_ir.prelude_functions
          ? package_ir.storage->functions().size() -
                package_ir.prelude_functions
          : 0;
  if (borrow::check_borrows(package_ir, ctx.bag).is_err() ||
      ctx.bag.has_errors()) {
    return base::make_err(diag::Reported{});
  }
  return base::make_ok(FrontendOutput{
      .package = std::move(package_ir),
      .module_count = module_count,
      .function_count = function_count,
  });
}

base::Result<analyzer::ModuleTree, diag::Reported> front_end_root(
    PipelineContext& ctx,
    source::FileId root) {
  const analyzer::ModuleInput single_input{"", root};
  base::Result<std::span<const analyzer::ModuleInput>, diag::Reported> prelude =
      std_prelude(ctx);
  if (prelude.is_err()) {
    return base::make_err(diag::Reported{});
  }
  return analyzer::resolve_modules(root, {&single_input, 1}, "", ctx.sources,
                                   ctx.ast, ctx.bag,
                                   std::move(prelude).unwrap());
}

}  // namespace pipeline
