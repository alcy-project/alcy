// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pipeline/check.h"

#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "analyzer/resolve.h"
#include "analyzer/types.h"
#include "borrow/borrow.h"
#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "lower/lower.h"
#include "path/path.h"
#include "pipeline/pipeline_context.h"
#include "pipeline/std_stage.h"
#include "pipeline/target.h"
#include "source/source.h"

namespace pipeline {

namespace {

base::Result<CheckResult, diag::Reported> fail() {
  return base::make_err(diag::Reported{});
}

}  // namespace

base::Result<CheckResult, diag::Reported> finish_check(
    PipelineContext& ctx,
    analyzer::ModuleTree tree,
    usize file_count) {
  base::Result<analyzer::CheckedPackage, diag::Reported> checked =
      analyzer::check_package(tree, TARGET_WIDTH, ctx.ast, ctx.bag);
  usize module_count = 0;
  usize function_count = 0;
  if (checked.is_err() || ctx.bag.has_errors()) {
    return fail();
  } else {
    analyzer::CheckedPackage package = std::move(checked).unwrap();
    // Prelude modules check with the package but read as toolchain
    // sources, so reported counts exclude them.
    module_count = package.modules.size() > tree.prelude_modules
                       ? package.modules.size() - tree.prelude_modules
                       : 0;
    base::Result<lower::LoweredPackage, diag::Reported> lowered =
        lower::lower_package(std::move(package), TARGET_WIDTH, ctx.ast,
                             ctx.strings, ctx.bag);
    if (lowered.is_err()) {
      return fail();
    } else {
      lower::LoweredPackage package_ir = std::move(lowered).unwrap();
      function_count =
          package_ir.storage->functions().size() > package_ir.prelude_functions
              ? package_ir.storage->functions().size() -
                    package_ir.prelude_functions
              : 0;
      if (borrow::check_borrows(package_ir, ctx.bag).is_err()) {
        return fail();
      }
    }
  }

  if (!ctx.bag.has_errors()) {
    return base::make_ok(CheckResult{
        .file_count = file_count,
        .module_count = module_count,
        .function_count = function_count,
    });
  }
  return fail();
}

base::Result<CheckResult, diag::Reported> check_single_file(
    PipelineContext& ctx,
    std::string_view target) {
  base::Result<source::FileId, source::SourceError> file =
      ctx.sources.load(target);
  if (file.is_err()) {
    const u32 index = ctx.bag.emit(diag::Severity::Error, PIPELINE_IO_ERROR,
                                   "cannot read '{}'", target);
    (void)index;
    return fail();
  }
  const source::FileId root = std::move(file).unwrap();
  const analyzer::ModuleInput single_input{"", root};
  base::Result<std::span<const analyzer::ModuleInput>, diag::Reported> prelude =
      std_prelude(ctx);
  if (prelude.is_err()) {
    return fail();
  }
  base::Result<analyzer::ModuleTree, diag::Reported> tree =
      analyzer::resolve_modules(root, {&single_input, 1}, "", ctx.sources,
                                ctx.ast, ctx.bag, std::move(prelude).unwrap());
  if (tree.is_err()) {
    return fail();
  }
  return finish_check(ctx, std::move(tree).unwrap(), 1);
}

base::Result<CheckResult, diag::Reported> check_package(
    PipelineContext& ctx,
    const path::Path& root,
    source::FileId manifest_file,
    std::string_view manifest_name) {
  base::Result<BinTarget, diag::Reported> target =
      resolve_package_target(ctx, root, manifest_file, manifest_name);
  if (target.is_err() || ctx.bag.has_errors()) {
    return fail();
  }
  BinTarget resolved = std::move(target).unwrap();
  return finish_check(ctx, resolved.tree, resolved.file_count);
}

}  // namespace pipeline
