// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#include "app/driver_main.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "analyzer/resolve.h"
#include "analyzer/types.h"
#include "app/driver_config.h"
#include "app/driver_context.h"
#include "app/init_handler.h"
#include "app/new_command.h"
#include "app/parse_args.h"
#include "app/parse_output.h"
#include "app/result_code.h"
#include "base/logger.h"
#include "debug/fatal.h"
#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "fpag/arg/parser.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/term/color_mode.h"
#include "fpag/term/color_style.h"
#include "fpag/term/console.h"
#include "ir/storage.h"
#include "ir/type.h"
#include "lower/lower.h"
#include "path/path.h"
#include "pipeline/pipeline.h"
#include "pkg/manifest.h"
#include "pkg/resolve.h"
#include "source/source.h"

namespace app {

namespace {

i32 run_build(const DriverConfig& config) {
  DriverContext ctx;
  // TODO: picks up --release (config.release) once codegen lands.
  (void)config.release;

  const std::string_view raw_dir =
      config.target_dir.empty() ? "." : config.target_dir;
  base::Result<path::Path, path::PathError> dir =
      path::Path::from_native(raw_dir);
  if (dir.is_err()) {
    const u32 index = ctx.bag.emit(diag::Severity::Error, kDriverIoError,
                                   "invalid target directory '{}'", raw_dir);
    (void)index;
    report(ctx.bag, ctx.sources);
    return result_code(ResultCode::BuildFailed);
  }
  const path::Path root = std::move(dir).unwrap();
  const path::Path manifest_path = root.join(pkg::kManifestFileName);

  base::Result<source::FileId, source::SourceError> manifest =
      ctx.sources.load(manifest_path.as_view());
  if (manifest.is_ok()) {
    diag::Fallible<std::vector<pkg::ResolvedPackage>> resolved =
        pkg::resolve_package(root.as_view(), ctx.sources, ctx.arena, ctx.bag);
    if (resolved.is_err()) {
      report(ctx.bag, ctx.sources);
      return result_code(ResultCode::BuildFailed);
    }
    diag::Fallible<pipeline::ProjectBuild> built = pipeline::compile_project(
        std::move(resolved).unwrap(), ctx.sources, ctx.bag);
    report(ctx.bag, ctx.sources);
    if (built.is_err() || ctx.bag.has_errors()) {
      return result_code(ResultCode::BuildFailed);
    }
    const pipeline::ProjectBuild build = std::move(built).unwrap();
    base::logger.wo_prefix("built {} file(s) from {} package(s)",
                           build.files_loaded, build.packages);
    return result_code(ResultCode::Success);
  }

  const u32 index = ctx.bag.emit(
      diag::Severity::Warning, kDriverNoManifest,
      "no manifest found at '{}'; building directory directly", raw_dir);
  (void)index;
  diag::Fallible<pipeline::DiscoveredSources> discovered =
      pipeline::discover_sources(root.as_view(), ctx.sources, ctx.bag);
  report(ctx.bag, ctx.sources);
  if (discovered.is_err() || ctx.bag.has_errors()) {
    return result_code(ResultCode::BuildFailed);
  }
  base::logger.wo_prefix("built {} file(s)",
                         std::move(discovered).unwrap().files.size());
  return result_code(ResultCode::Success);
}

i32 check_package(DriverContext& ctx,
                  const path::Path& root,
                  source::FileId manifest_file,
                  std::string_view manifest_name);

// MVP pointer width: isize/usize map to 64-bit integers. An explicit
// choice (never sniffed from the host); a --target flag selects it
// once cross builds land.
constexpr ir::PointerWidth kCheckWidth = ir::PointerWidth::W64;

// Runs type checking over a resolved tree, reports diagnostics, and
// maps the outcome to an exit code. Shared by manifest and
// single-file modes.
i32 finish_check(DriverContext& ctx,
                 analyzer::ModuleTree tree,
                 usize file_count) {
  diag::Fallible<analyzer::CheckedPackage> checked =
      analyzer::check_package(tree, kCheckWidth, ctx.bag);
  if (checked.is_err()) {
    report(ctx.bag, ctx.sources);
    return result_code(ResultCode::CheckFailed);
  }
  if (ctx.bag.has_errors()) {
    report(ctx.bag, ctx.sources);
    return result_code(ResultCode::CheckFailed);
  }
  analyzer::CheckedPackage package = std::move(checked).unwrap();
  const usize modules = package.modules.size();
  diag::Fallible<ir::Storage> lowered = lower::lower_package(
      std::move(package), kCheckWidth, ctx.strings, ctx.bag);
  if (lowered.is_err()) {
    report(ctx.bag, ctx.sources);
    return result_code(ResultCode::CheckFailed);
  }
  report(ctx.bag, ctx.sources);
  if (ctx.bag.has_errors()) {
    return result_code(ResultCode::CheckFailed);
  }
  base::logger.wo_prefix("checked {} file(s), {} module(s), {} function(s)",
                         file_count, modules,
                         std::move(lowered).unwrap().functions().size());
  return result_code(ResultCode::Success);
}

i32 run_check(const DriverConfig& config) {
  DriverContext ctx;
  const std::string_view raw_target =
      config.target_dir.empty() ? "." : config.target_dir;
  base::Result<path::Path, path::PathError> target =
      path::Path::from_native(raw_target);
  if (target.is_err()) {
    const u32 index = ctx.bag.emit(diag::Severity::Error, kDriverIoError,
                                   "invalid target '{}'", raw_target);
    (void)index;
    report(ctx.bag, ctx.sources);
    return result_code(ResultCode::CheckFailed);
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
        diag::Severity::Error, kDriverNoManifest,
        "no manifest found at '{}'; check a file or add alcy.toml", raw_target);
    (void)index;
    report(ctx.bag, ctx.sources);
    return result_code(ResultCode::CheckFailed);
  }
  base::Result<source::FileId, source::SourceError> file =
      ctx.sources.load(target_path.as_view());
  if (file.is_err()) {
    const u32 index = ctx.bag.emit(diag::Severity::Error, kDriverIoError,
                                   "cannot read '{}'", raw_target);
    (void)index;
    report(ctx.bag, ctx.sources);
    return result_code(ResultCode::CheckFailed);
  }
  const source::FileId root = std::move(file).unwrap();
  const std::vector<source::FileId> files{root};
  diag::Fallible<analyzer::ModuleTree> tree = analyzer::resolve_modules(
      root, files, "", ctx.sources, ctx.arena, ctx.bag);
  if (tree.is_err()) {
    report(ctx.bag, ctx.sources);
    return result_code(ResultCode::CheckFailed);
  }
  if (ctx.bag.has_errors()) {
    report(ctx.bag, ctx.sources);
    return result_code(ResultCode::CheckFailed);
  }
  return finish_check(ctx, std::move(tree).unwrap(), files.size());
}

i32 check_package(DriverContext& ctx,
                  const path::Path& root,
                  source::FileId manifest_file,
                  std::string_view manifest_name) {
  diag::Fallible<pkg::PackageManifest> parsed =
      pkg::parse_manifest(ctx.sources.bytes(manifest_file), manifest_name,
                          manifest_file, ctx.bag, ctx.arena);
  if (parsed.is_err()) {
    report(ctx.bag, ctx.sources);
    return result_code(ResultCode::CheckFailed);
  }
  const pkg::PackageManifest manifest = std::move(parsed).unwrap();
  if (manifest.bin_count == 0) {
    const u32 index = ctx.bag.emit(diag::Severity::Error, kDriverNoTargets,
                                   "manifest '{}' declares no [[bin]] targets",
                                   manifest_name);
    (void)index;
    report(ctx.bag, ctx.sources);
    return result_code(ResultCode::CheckFailed);
  }
  if (manifest.bin_count > 1) {
    const u32 index = ctx.bag.emit(
        diag::Severity::Error, kDriverNoTargets,
        "manifest '{}' declares {} [[bin]] targets; only one is supported",
        manifest_name, manifest.bin_count);
    (void)index;
    report(ctx.bag, ctx.sources);
    return result_code(ResultCode::CheckFailed);
  }

  diag::Fallible<pipeline::DiscoveredSources> discovered =
      pipeline::discover_sources(root.as_view(), ctx.sources, ctx.bag);
  if (discovered.is_err()) {
    report(ctx.bag, ctx.sources);
    return result_code(ResultCode::CheckFailed);
  }
  pipeline::DiscoveredSources found = std::move(discovered).unwrap();
  const std::vector<source::FileId>& files = found.files;
  const path::Path bin_path = root.join(manifest.bins[0].path);
  source::FileId bin_file = source::kUnknownFile;
  for (source::FileId id : files) {
    if (ctx.sources.name(id) == bin_path.as_view()) {
      bin_file = id;
      break;
    }
  }
  if (bin_file == source::kUnknownFile) {
    const u32 index = ctx.bag.emit(diag::Severity::Error, kDriverNoTargets,
                                   "bin target '{}' was not discovered",
                                   manifest.bins[0].path);
    (void)index;
    report(ctx.bag, ctx.sources);
    return result_code(ResultCode::CheckFailed);
  }

  diag::Fallible<analyzer::ModuleTree> tree = analyzer::resolve_modules(
      bin_file, files, manifest.name, ctx.sources, ctx.arena, ctx.bag);
  if (tree.is_err()) {
    report(ctx.bag, ctx.sources);
    return result_code(ResultCode::CheckFailed);
  }
  if (ctx.bag.has_errors()) {
    report(ctx.bag, ctx.sources);
    return result_code(ResultCode::CheckFailed);
  }
  return finish_check(ctx, std::move(tree).unwrap(), files.size());
}

i32 not_implemented(std::string_view subcommand) {
  DriverContext ctx;
  const u32 index =
      ctx.bag.emit(diag::Severity::Error, kDriverNotImplemented,
                   "'alcy {}' is not implemented yet", subcommand);
  (void)index;
  report(ctx.bag, ctx.sources);
  return result_code(ResultCode::NotImplemented);
}

i32 dispatch(const DriverConfig& config) {
  switch (config.subcommand) {
    case Subcommand::Build: return run_build(config);
    case Subcommand::New: return run_new(config.target_dir);
    case Subcommand::Test: return not_implemented("test");
    case Subcommand::Run: return not_implemented("run");
    case Subcommand::Check: return run_check(config);
    case Subcommand::None: break;
  }
  // parse_args maps a missing subcommand to NoSubcommand/UnknownSubcommand,
  // so only interruption outcomes carry Subcommand::None here.
  UNREACHABLE();
}

i32 run_interruption(arg::Parser& parser,
                     const ParseOutcome& outcome,
                     ResultCode code,
                     i32 argc,
                     const char* const* argv) {
  const term::ColorStyle style = term::console_color_style(
      term::Stream::Stdout, scan_color_mode(argc, argv));
  base::init_logger(style);
  const std::string text = render_outcome(parser, outcome, style);
  if (!text.empty()) {
    base::logger.wo_prefix("{}", text);
  }
  return result_code(code);
}

}  // namespace

i32 driver_main(i32 argc, char** argv) {
  init_runtime();

  arg::Parser parser = build_parser();
  ParseOutcome outcome = parse_args(parser, argc, argv);
  if (const std::optional<ResultCode> code = interruption_exit_code(outcome)) {
    return run_interruption(parser, outcome, *code, argc, argv);
  }
  const DriverConfig& config = outcome.get<DriverConfig>();
  base::init_logger(
      term::console_color_style(term::Stream::Stdout, config.color_mode));
  return dispatch(config);
}

}  // namespace app
