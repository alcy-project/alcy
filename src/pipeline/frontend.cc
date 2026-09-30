// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pipeline/frontend.h"

#include <span>
#include <utility>

#include "analyzer/resolve.h"
#include "analyzer/types.h"
#include "borrow/borrow.h"
#include "diag/bag.h"
#include "fpag/base/result.h"
#include "fpag/debug/profiler/profile_scope.h"
#include "lower/lower.h"
#include "pipeline/embedded_std.h"
#include "pipeline/pipeline_context.h"
#include "pipeline/std_select.h"
#include "pipeline/std_stage.h"
#include "pipeline/target.h"
#include "source/source.h"

namespace pipeline {

base::Result<FrontendOutput, diag::Reported> run_frontend(
    PipelineContext& ctx,
    analyzer::ModuleTree tree) {
  base::Result<analyzer::CheckedPackage, diag::Reported> checked = [&] {
    PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(ctx.profiler, "analyze",
                                             "frontend");
    const std::span<const analyzer::StdHint> hints(STD_HINTS, STD_HINT_COUNT);
    return analyzer::check_package(tree, TARGET_WIDTH, ctx.ast, ctx.bag, hints);
  }();
  if (checked.is_err() || ctx.bag.has_errors()) {
    return base::make_err(diag::Reported{});
  }
  analyzer::CheckedPackage package = std::move(checked).unwrap();
  // Staged prelude modules check with the package but read as
  // toolchain sources, so reported counts exclude them.
  const usize module_count = package.modules.size() > tree.staged_modules
                                 ? package.modules.size() - tree.staged_modules
                                 : 0;
  base::Result<lower::LoweredPackage, diag::Reported> lowered = [&] {
    PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(ctx.profiler, "lower", "frontend");
    return lower::lower_package(std::move(package), TARGET_WIDTH, ctx.ast,
                                ctx.strings, ctx.bag);
  }();
  if (lowered.is_err() || ctx.bag.has_errors()) {
    return base::make_err(diag::Reported{});
  }
  lower::LoweredPackage package_ir = std::move(lowered).unwrap();
  const usize function_count =
      package_ir.storage->functions().size() > package_ir.prelude_functions
          ? package_ir.storage->functions().size() -
                package_ir.prelude_functions
          : 0;
  const bool borrows_ok = [&] {
    PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(ctx.profiler, "borrow",
                                             "frontend");
    return borrow::check_borrows(package_ir, ctx.bag).is_ok();
  }();
  if (!borrows_ok || ctx.bag.has_errors()) {
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
    source::FileId root,
    const StdSelection& selection) {
  const analyzer::ModuleInput single_input{"", root};
  const std::span<const analyzer::ModuleInput> prelude = [&] {
    PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(ctx.profiler, "prelude",
                                             "frontend");
    return std_prelude(ctx, selection);
  }();
  return [&] {
    PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(ctx.profiler, "resolve",
                                             "frontend");
    const std::span<const analyzer::StdHint> hints(STD_HINTS, STD_HINT_COUNT);
    return analyzer::resolve_modules(root, {&single_input, 1}, "", ctx.sources,
                                     ctx.ast, ctx.bag, prelude, hints);
  }();
}

}  // namespace pipeline
