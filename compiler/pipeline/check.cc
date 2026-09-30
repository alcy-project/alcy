// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pipeline/check.h"

#include <string_view>
#include <utility>

#include "analyzer/resolve.h"
#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/debug/profiler/profile_scope.h"
#include "i18n/messages.h"
#include "path/path.h"
#include "pipeline/frontend.h"
#include "pipeline/pipeline_context.h"
#include "pipeline/std_select.h"
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
  base::Result<FrontendOutput, diag::Reported> out = run_frontend(ctx, tree);
  if (out.is_err() || ctx.bag.has_errors()) {
    return fail();
  }
  FrontendOutput done = std::move(out).unwrap();
  return base::make_ok(CheckResult{
      .file_count = file_count,
      .module_count = done.module_count,
      .function_count = done.function_count,
  });
}

base::Result<CheckResult, diag::Reported> check_single_file(
    PipelineContext& ctx,
    std::string_view target) {
  base::Result<source::FileId, source::SourceError> file = [&] {
    PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(ctx.profiler, "load", "frontend");
    return ctx.sources.load(target);
  }();
  if (file.is_err()) {
    const u32 index = ctx.bag.emit<i18n::Key::PipelineCannotRead>(
        diag::Severity::Error, PIPELINE_IO_ERROR, target);
    (void)index;
    return fail();
  }
  return check_root(ctx, std::move(file).unwrap());
}

base::Result<CheckResult, diag::Reported> check_source(PipelineContext& ctx,
                                                       std::string_view name,
                                                       std::string_view bytes) {
  return check_root(ctx, ctx.sources.add_virtual(name, bytes));
}

// Single-file checks name the whole suite: there is no manifest to
// select from, and no flags to narrow it with.
base::Result<CheckResult, diag::Reported> check_root(PipelineContext& ctx,
                                                     source::FileId root) {
  base::Result<analyzer::ModuleTree, diag::Reported> tree =
      front_end_root(ctx, root, full_std_selection());
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
