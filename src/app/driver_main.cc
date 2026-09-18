// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#include "app/driver_main.h"

#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "app/driver_config.h"
#include "app/init_handler.h"
#include "app/parse_args.h"
#include "app/result_code.h"
#include "base/logger.h"
#include "cfg/build_config.h"
#include "debug/fatal.h"
#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "diag/render.h"
#include "fmt/format.h"
#include "fpag/arg/parser.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/mem/arena.h"
#include "fpag/term/color_mode.h"
#include "fpag/term/console.h"
#include "pipeline/pipeline.h"
#include "pkg/manifest.h"
#include "pkg/resolve.h"
#include "source/source.h"

#if BUILD_FLAG(IS_OS_WIN)
#include <direct.h>
#else
#include <sys/stat.h>
#endif

namespace app {

namespace {

// Diagnostic codes 3000-3099 are reserved for the driver.
constexpr u32 kDriverNoManifest = 3000;
constexpr u32 kDriverIoError = 3001;
constexpr u32 kDriverNotImplemented = 3002;

struct DriverContext {
  mem::Arena arena;
  source::SourceManager sources;
  diag::DiagBag bag;

  DriverContext() : bag(arena) { arena.reserve(1u << 20); }
};

void report(const diag::DiagBag& bag, const source::SourceManager& sources) {
  bag.for_each([&](const diag::Diagnostic& diag) {
    fmt::memory_buffer out;
    diag::render(diag, out, {}, pipeline::fetch_source, &sources);
    base::logger.wo_prefix("{}", std::string_view(out.data(), out.size()));
  });
}

bool make_dirs(const std::string& path) {
  std::string current;
  for (usize i = 0; i <= path.size(); ++i) {
    if (i == path.size() || path[i] == '/') {
      if (!current.empty()) {
#if BUILD_FLAG(IS_OS_WIN)
        ::_mkdir(current.c_str());
#else
        ::mkdir(current.c_str(), 0755);
#endif
      }
    }
    if (i < path.size()) {
      current.push_back(path[i]);
    }
  }
  return true;
}

bool write_text_file(const std::string& path, std::string_view content) {
  std::FILE* file = std::fopen(path.c_str(), "wb");
  if (file == nullptr) {
    return false;
  }
  const usize written = std::fwrite(content.data(), 1, content.size(), file);
  std::fclose(file);
  return written == content.size();
}

i32 run_build(const DriverConfig& config) {
  DriverContext ctx;
  // TODO: picks up --release (config.release) once codegen lands.
  (void)config.release;

  const std::string_view dir =
      config.target_dir.empty() ? "." : config.target_dir;
  const std::string manifest_path =
      std::string(dir) + "/" + std::string(pkg::kManifestFileName);

  base::Result<source::FileId, source::SourceError> manifest =
      ctx.sources.load(manifest_path);
  if (manifest.is_ok()) {
    diag::Fallible<std::vector<pkg::ResolvedPackage>> resolved =
        pkg::resolve_package(dir, ctx.sources, ctx.arena, ctx.bag);
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
      "no manifest found at '{}'; building directory directly", dir);
  (void)index;
  diag::Fallible<pipeline::DiscoveredSources> discovered =
      pipeline::discover_sources(dir, ctx.sources, ctx.bag);
  report(ctx.bag, ctx.sources);
  if (discovered.is_err() || ctx.bag.has_errors()) {
    return result_code(ResultCode::BuildFailed);
  }
  base::logger.wo_prefix("built {} file(s)",
                         std::move(discovered).unwrap().files.size());
  return result_code(ResultCode::Success);
}

bool valid_package_name(std::string_view name) {
  if (name.empty()) {
    return false;
  }
  for (const char c : name) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '_' || c == '-';
    if (!ok) {
      return false;
    }
  }
  return true;
}

i32 run_new(const DriverConfig& config) {
  if (!valid_package_name(config.target_dir)) {
    base::logger.wo_prefix("invalid package name '{}'; use [A-Za-z0-9_-] only",
                           config.target_dir);
    return result_code(ResultCode::ArgParseError);
  }

  DriverContext ctx;
  const std::string root(config.target_dir);
  const std::string src_dir = root + "/src";
  const std::string manifest_path =
      root + "/" + std::string(pkg::kManifestFileName);
  const std::string main_path = src_dir + "/main.alcy";

  const std::string manifest_text =
      "[package]\nname = \"" + root + "\"\nversion = \"0.1.0\"\n";
  static constexpr std::string_view kMainText = "// Write your code here.\n";
  if (!make_dirs(src_dir) || !write_text_file(manifest_path, manifest_text) ||
      !write_text_file(main_path, kMainText)) {
    const u32 index = ctx.bag.emit(diag::Severity::Error, kDriverIoError,
                                   "cannot create package '{}'", root);
    (void)index;
    report(ctx.bag, ctx.sources);
    return result_code(ResultCode::BuildFailed);
  }
  base::logger.wo_prefix("created package '{}'", root);
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

i32 dispatch(ParseArgsResult&& args_result) {
  switch (args_result.tag()) {
    case ParseArgsResult::TagOf<DriverConfig>: {
      DriverConfig config = std::move(args_result).get<DriverConfig>();
      switch (config.subcommand) {
        case Subcommand::Build: return run_build(config);
        case Subcommand::New: return run_new(config);
        case Subcommand::Test: return not_implemented("test");
        case Subcommand::Run: return not_implemented("run");
        case Subcommand::Check: return not_implemented("check");
        case Subcommand::None: break;
      }
      UNREACHABLE();
    }
    case ParseArgsResult::TagOf<ParseInterruptedReason>: {
      switch (std::move(args_result).get<ParseInterruptedReason>()) {
        case ParseInterruptedReason::ParseError: {
          return result_code(ResultCode::ArgParseError);
        }
        case ParseInterruptedReason::UnknownSubcommand: {
          return result_code(ResultCode::ArgParseError);
        }
        case ParseInterruptedReason::HelpRequested: {
          return result_code(ResultCode::Success);
        }
        case ParseInterruptedReason::VersionRequested: {
          return result_code(ResultCode::Success);
        }
      }
    }
    default: UNREACHABLE();
  }
}

}  // namespace

i32 driver_main(i32 argc, char** argv) {
  init();

  arg::Parser parser = build_parser();
  ParseArgsResult args_result = parse_args(
      std::move(parser), argc, argv,
      term::console_color_style(term::Stream::Stdout, term::ColorMode::Auto));
  return dispatch(std::move(args_result));
}

}  // namespace app
