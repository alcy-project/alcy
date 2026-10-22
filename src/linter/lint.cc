// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "linter/lint.h"

#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "analyzer/resolve.h"
#include "analyzer/types.h"
#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "diag/stage.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "i18n/messages.h"
#include "linter/config.h"
#include "linter/engine.h"
#include "linter/lint.h"
#include "path/path.h"
#include "pipeline/diag_code.h"
#include "pipeline/frontend.h"
#include "pipeline/pipeline_context.h"
#include "pipeline/std_select.h"
#include "pipeline/target.h"
#include "pkg/config.h"
#include "pkg/manifest.h"
#include "source/source.h"

namespace linter {

namespace {

base::Result<LintOutcome, diag::Reported> fail() {
  return base::make_err(diag::Reported{});
}

// Reports a file the linter could not load the way the single-file
// check reports one, so the two verbs name the same failure the same
// way.
base::Result<LintOutcome, diag::Reported> report_load_failure(
    diag::DiagBag& bag,
    source::SourceError reason,
    std::string_view target) {
  if (reason == source::SourceError::NotRegular) {
    const u32 index = bag.emit<i18n::Key::PipelineSourceNotRegular>(
        diag::Severity::Error, diag::Stage::Pipeline,
        pipeline::DiagCode::SourceNotRegular, target);
    (void)index;
  } else {
    const u32 index = bag.emit<i18n::Key::PipelineCannotRead>(
        diag::Severity::Error, diag::Stage::Pipeline,
        pipeline::DiagCode::IoError, target);
    (void)index;
  }
  return fail();
}

// The shared tail of the two single-file entries: everything from a
// loaded root file onwards, which does not care where the bytes came
// from.
base::Result<LintOutcome, diag::Reported> lint_root(
    pipeline::PipelineContext& ctx,
    source::FileId root,
    const LintConfig& config) {
  base::Result<analyzer::ModuleTree, diag::Reported> tree =
      pipeline::front_end_root(ctx, root, pipeline::full_std_selection());
  if (tree.is_err() || ctx.bag.has_errors()) {
    return fail();
  }
  base::Result<analyzer::CheckedPackage, diag::Reported> checked =
      pipeline::analyze_tree(ctx, std::move(tree).unwrap());
  if (checked.is_err() || ctx.bag.has_errors()) {
    return fail();
  }
  analyzer::CheckedPackage package = std::move(checked).unwrap();
  const LintStats stats = run_lints(ctx, package, config);
  // Lowering and the borrow check run after the rules so a program the
  // compiler would refuse is not reported lint-clean. Their errors are
  // new; the findings already in the bag are not theirs to judge.
  base::Result<pipeline::FrontendOutput, diag::Reported> finished =
      pipeline::finish_frontend(ctx, std::move(package));
  if (finished.is_err()) {
    return fail();
  }
  const pipeline::FrontendOutput out = std::move(finished).unwrap();
  return base::make_ok(LintOutcome{.file_count = 1,
                                   .module_count = out.module_count,
                                   .warning_count = stats.warnings,
                                   .error_count = stats.errors});
}

}  // namespace

base::Result<LintConfig, diag::Reported> load_config(
    pipeline::PipelineContext& ctx,
    const path::Path& package_root) {
  const path::Path path =
      package_root.join(pkg::CONFIG_DIR_NAME).join(LINT_FILE_NAME);
  base::Result<source::FileId, source::SourceError> file =
      ctx.sources.load(path.as_view());
  if (file.is_err()) {
    // An absent file is the defaults, and an empty TOML document parses
    // to exactly those, so the two need no second path.
    return parse_lint_config("", path.as_view(), source::UNKNOWN_FILE, ctx.bag,
                             ctx.arena);
  }
  const source::FileId loaded = std::move(file).unwrap();
  const std::optional<std::string_view> bytes = ctx.sources.bytes(loaded);
  if (!bytes.has_value()) {
    return parse_lint_config("", path.as_view(), loaded, ctx.bag, ctx.arena);
  }
  return parse_lint_config(*bytes, path.as_view(), loaded, ctx.bag, ctx.arena);
}

base::Result<LintOutcome, diag::Reported> lint_single_file(
    pipeline::PipelineContext& ctx,
    std::string_view target,
    const LintConfig& config) {
  base::Result<source::FileId, source::SourceError> file =
      ctx.sources.load(target);
  if (file.is_err()) {
    return report_load_failure(ctx.bag, std::move(file).unwrap_err(), target);
  }
  return lint_root(ctx, std::move(file).unwrap(), config);
}

base::Result<LintOutcome, diag::Reported> lint_source(
    pipeline::PipelineContext& ctx,
    std::string_view name,
    std::string_view bytes,
    const LintConfig& config) {
  return lint_root(ctx, ctx.sources.add_virtual(name, bytes), config);
}

base::Result<LintOutcome, diag::Reported> lint_package(
    pipeline::PipelineContext& ctx,
    const path::Path& root,
    source::FileId manifest_file,
    std::string_view manifest_name,
    const LintConfig& config) {
  base::Result<std::vector<pipeline::PackageTarget>, diag::Reported> targets =
      pipeline::resolve_package_targets(ctx, root, manifest_file, manifest_name,
                                        pipeline::TargetScope::All);
  if (targets.is_err() || ctx.bag.has_errors()) {
    return fail();
  }
  std::vector<pipeline::PackageTarget> resolved = std::move(targets).unwrap();
  LintOutcome total{};
  bool ok = true;
  for (pipeline::PackageTarget& target : resolved) {
    base::Result<analyzer::CheckedPackage, diag::Reported> checked =
        pipeline::analyze_tree(ctx, target.tree);
    if (checked.is_err() || ctx.bag.has_errors()) {
      ok = false;
      continue;
    }
    analyzer::CheckedPackage package = std::move(checked).unwrap();
    const LintStats stats = run_lints(ctx, package, config);
    base::Result<pipeline::FrontendOutput, diag::Reported> finished =
        pipeline::finish_frontend(ctx, std::move(package));
    if (finished.is_err()) {
      ok = false;
      continue;
    }
    const pipeline::FrontendOutput out = std::move(finished).unwrap();
    total.file_count += target.file_count;
    total.module_count += out.module_count;
    total.warning_count += stats.warnings;
    total.error_count += stats.errors;
  }
  // Several targets analyze shared modules, so one finding can be
  // reported once per tree; the run renders it once.
  ctx.bag.dedup();
  if (!ok) {
    return fail();
  }
  return base::make_ok(total);
}

base::Result<LintOutcome, diag::Reported> lint_suite(
    pipeline::PipelineContext& ctx,
    const path::Path& root,
    source::FileId manifest_file,
    std::string_view manifest_name,
    const LintConfig& config) {
  const std::optional<std::string_view> bytes =
      ctx.sources.bytes(manifest_file);
  if (!bytes.has_value()) {
    const u32 index = ctx.bag.emit<i18n::Key::PipelineManifestNotLoaded>(
        diag::Severity::Error, diag::Stage::Pipeline,
        pipeline::DiagCode::IoError, manifest_name);
    (void)index;
    return fail();
  }
  base::Result<pkg::SuiteManifest, diag::Reported> parsed =
      pkg::parse_suite_manifest(*bytes, manifest_name, manifest_file, ctx.bag,
                                ctx.arena);
  if (parsed.is_err()) {
    return fail();
  }
  const pkg::SuiteManifest suite = std::move(parsed).unwrap();
  base::Result<void, pkg::SuiteError> verified =
      pkg::verify_suite_manifest(suite);
  if (verified.is_err()) {
    pkg::report_suite_error(std::move(verified).unwrap_err(), manifest_name,
                            ctx.bag);
    return fail();
  }
  if (suite.package_count == 0) {
    const u32 index = ctx.bag.emit<i18n::Key::PipelineSuiteEmpty>(
        diag::Severity::Error, diag::Stage::Pipeline,
        pipeline::DiagCode::NoTargets, manifest_name);
    (void)index;
    return fail();
  }
  LintOutcome total{};
  for (u32 i = 0; i < suite.package_count; ++i) {
    base::Result<pipeline::ManifestProbe, diag::Reported> probe =
        pipeline::require_package_manifest(
            ctx, root.join(suite.packages[i]).as_view(), false);
    if (probe.is_err()) {
      return fail();
    }
    pipeline::ManifestProbe member = std::move(probe).unwrap();
    base::Result<LintOutcome, diag::Reported> one = lint_package(
        ctx, member.root, member.manifest, member.manifest_name, config);
    if (one.is_err()) {
      return fail();
    }
    const LintOutcome counted = std::move(one).unwrap();
    total.file_count += counted.file_count;
    total.module_count += counted.module_count;
    total.warning_count += counted.warning_count;
    total.error_count += counted.error_count;
  }
  return base::make_ok(total);
}

}  // namespace linter
