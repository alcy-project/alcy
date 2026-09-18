// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#include "app/driver_main.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
    case Subcommand::Check: return not_implemented("check");
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
