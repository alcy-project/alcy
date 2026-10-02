// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pipeline/check.h"

#include <string_view>
#include <utility>
#include <vector>

#include "analyzer/resolve.h"
#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "diag/stage.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/debug/profiler/profile_scope.h"
#include "i18n/messages.h"
#include "path/path.h"
#include "pipeline/diag_code.h"
#include "pipeline/frontend.h"
#include "pipeline/pipeline_context.h"
#include "pipeline/std_select.h"
#include "pipeline/target.h"
#include "source/source.h"

namespace pipeline {

namespace {

base::Result<CheckOutcome, diag::Reported> fail() {
  return base::make_err(diag::Reported{});
}

}  // namespace

base::Result<CheckOutcome, diag::Reported> finish_check(
    PipelineContext& ctx,
    analyzer::ModuleTree tree,
    usize file_count) {
  base::Result<FrontendOutput, diag::Reported> out = run_frontend(ctx, tree);
  if (out.is_err() || ctx.bag.has_errors()) {
    return fail();
  }
  FrontendOutput done = std::move(out).unwrap();
  return base::make_ok(CheckOutcome{
      .file_count = file_count,
      .module_count = done.module_count,
      .function_count = done.function_count,
  });
}

base::Result<CheckOutcome, diag::Reported> check_single_file(
    PipelineContext& ctx,
    std::string_view target) {
  base::Result<source::FileId, source::SourceError> file = [&] {
    PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(ctx.profiler, "load", "frontend");
    return ctx.sources.load(target);
  }();
  if (file.is_err()) {
    const u32 index = ctx.bag.emit<i18n::Key::PipelineCannotRead>(
        diag::Severity::Error, diag::Stage::Pipeline, DiagCode::IoError,
        target);
    (void)index;
    return fail();
  }
  return check_root(ctx, std::move(file).unwrap());
}

base::Result<CheckOutcome, diag::Reported> check_source(
    PipelineContext& ctx,
    std::string_view name,
    std::string_view bytes) {
  return check_root(ctx, ctx.sources.add_virtual(name, bytes));
}

// Single-file checks name the whole suite: there is no manifest to
// select from, and no flags to narrow it with.
base::Result<CheckOutcome, diag::Reported> check_root(PipelineContext& ctx,
                                                      source::FileId root) {
  base::Result<analyzer::ModuleTree, diag::Reported> tree =
      front_end_root(ctx, root, full_std_selection());
  if (tree.is_err()) {
    return fail();
  }
  return finish_check(ctx, std::move(tree).unwrap(), 1);
}

base::Result<CheckOutcome, diag::Reported> check_package(
    PipelineContext& ctx,
    const path::Path& root,
    source::FileId manifest_file,
    std::string_view manifest_name) {
  base::Result<std::vector<PackageTarget>, diag::Reported> targets =
      resolve_package_targets(ctx, root, manifest_file, manifest_name,
                              TargetScope::All);
  if (targets.is_err() || ctx.bag.has_errors()) {
    return fail();
  }
  std::vector<PackageTarget> resolved = std::move(targets).unwrap();
  // Every target draws on the same module selection but roots a different
  // tree, and `package::` resolves at the root: an error in one tree is
  // invisible from another, so every tree is analyzed. A tree that
  // follows a failed one still reports its own analysis. Identical
  // findings from shared modules collapse below into one rendering.
  bool ok = true;
  bool have_outcome = false;
  CheckOutcome outcome{};
  for (PackageTarget& target : resolved) {
    base::Result<CheckOutcome, diag::Reported> one =
        finish_check(ctx, target.tree, target.file_count);
    if (one.is_err() || ctx.bag.has_errors()) {
      ok = false;
      continue;
    }
    // Every tree counts the same selection, so the first success
    // measures the package; summing would count its files twice.
    if (!have_outcome) {
      outcome = std::move(one).unwrap();
      have_outcome = true;
    }
  }
  ctx.bag.dedup();
  if (!ok || ctx.bag.has_errors()) {
    return fail();
  }
  return base::make_ok(outcome);
}

}  // namespace pipeline
