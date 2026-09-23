// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/check_command.h"

#include <string_view>
#include <utility>
#include <vector>

#include "analyzer/resolve.h"
#include "analyzer/types.h"
#include "base/logger.h"
#include "borrow/borrow.h"
#include "cli/cli_config.h"
#include "cli/cli_context.h"
#include "cli/result_code.h"
#include "cli/target.h"
#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "lower/lower.h"
#include "path/path.h"
#include "pkg/manifest.h"
#include "source/source.h"

namespace cli {

namespace {

ResultCode check_package(CliContext& ctx,
                         const path::Path& root,
                         source::FileId manifest_file,
                         std::string_view manifest_name);

// Runs type checking over a resolved tree, reports diagnostics, and
// maps the outcome to an exit code. Shared by manifest and
// single-file modes.
ResultCode finish_check(CliContext& ctx,
                        analyzer::ModuleTree tree,
                        usize file_count) {
  diag::Fallible<analyzer::CheckedPackage> checked =
      analyzer::check_package(tree, kCheckWidth, ctx.ast, ctx.bag);
  if (checked.is_err()) {
    report(ctx.bag, ctx.sources);
    return ResultCode::CheckFailed;
  }
  if (ctx.bag.has_errors()) {
    report(ctx.bag, ctx.sources);
    return ResultCode::CheckFailed;
  }
  analyzer::CheckedPackage package = std::move(checked).unwrap();
  const usize modules = package.modules.size();
  diag::Fallible<lower::LoweredPackage> lowered = lower::lower_package(
      std::move(package), kCheckWidth, ctx.ast, ctx.strings, ctx.bag);
  if (lowered.is_err()) {
    report(ctx.bag, ctx.sources);
    return ResultCode::CheckFailed;
  }
  lower::LoweredPackage package_ir = std::move(lowered).unwrap();
  const usize functions = package_ir.storage.functions().size();
  borrow::check_borrows(package_ir, ctx.bag);
  report(ctx.bag, ctx.sources);
  if (ctx.bag.has_errors()) {
    return ResultCode::CheckFailed;
  }
  base::logger.wo_prefix("checked {} file(s), {} module(s), {} function(s)",
                         file_count, modules, functions);
  return ResultCode::Success;
}

ResultCode check_package(CliContext& ctx,
                         const path::Path& root,
                         source::FileId manifest_file,
                         std::string_view manifest_name) {
  diag::Fallible<pkg::PackageManifest> parsed =
      pkg::parse_manifest(ctx.sources.bytes(manifest_file), manifest_name,
                          manifest_file, ctx.bag, ctx.arena);
  if (parsed.is_err()) {
    return ResultCode::CheckFailed;
  }
  const pkg::PackageManifest manifest = std::move(parsed).unwrap();

  diag::Fallible<BinTarget> target =
      resolve_bin_target(ctx, root, manifest, manifest_name);
  if (target.is_err() || ctx.bag.has_errors()) {
    report(ctx.bag, ctx.sources);
    return ResultCode::CheckFailed;
  }
  BinTarget resolved = std::move(target).unwrap();
  return finish_check(ctx, resolved.tree, resolved.file_count);
}

}  // namespace

ResultCode run_check(const CliConfig& config) {
  CliContext ctx;
  const std::string_view raw_target =
      config.target_dir.empty() ? "." : config.target_dir;
  base::Result<path::Path, path::PathError> target =
      path::Path::from_native(raw_target);
  if (target.is_err()) {
    const u32 index = ctx.bag.emit(diag::Severity::Error, kCliIoError,
                                   "invalid target '{}'", raw_target);
    (void)index;
    report(ctx.bag, ctx.sources);
    return ResultCode::CheckFailed;
  }
  const path::Path target_path = std::move(target).unwrap();

  const path::Path manifest_path = target_path.join(pkg::kManifestFileName);
  base::Result<source::FileId, source::SourceError> manifest =
      ctx.sources.load(manifest_path.as_view());
  if (manifest.is_ok()) {
    return check_package(ctx, target_path, std::move(manifest).unwrap(),
                         manifest_path.as_view());
  }

  // Single-file mode for explicit `foo.al` targets. Directories without
  // a manifest are not checked: module structure needs declared roots.
  if (raw_target.size() < 4 ||
      raw_target.substr(raw_target.size() - 3) != ".al") {
    const u32 index = ctx.bag.emit(
        diag::Severity::Error, kCliNoManifest,
        "no manifest found at '{}'; check a file or add alcy.toml", raw_target);
    (void)index;
    report(ctx.bag, ctx.sources);
    return ResultCode::CheckFailed;
  }
  base::Result<source::FileId, source::SourceError> file =
      ctx.sources.load(target_path.as_view());
  if (file.is_err()) {
    const u32 index = ctx.bag.emit(diag::Severity::Error, kCliIoError,
                                   "cannot read '{}'", raw_target);
    (void)index;
    report(ctx.bag, ctx.sources);
    return ResultCode::CheckFailed;
  }
  const source::FileId root = std::move(file).unwrap();
  const analyzer::ModuleInput single_input{"", root};
  diag::Fallible<analyzer::ModuleTree> tree = analyzer::resolve_modules(
      root, {&single_input, 1}, "", ctx.sources, ctx.ast, ctx.bag);
  if (tree.is_err()) {
    report(ctx.bag, ctx.sources);
    return ResultCode::CheckFailed;
  }
  if (ctx.bag.has_errors()) {
    report(ctx.bag, ctx.sources);
    return ResultCode::CheckFailed;
  }
  return finish_check(ctx, std::move(tree).unwrap(), 1);
}

}  // namespace cli
