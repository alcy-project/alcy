// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/check_command.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/logger.h"
#include "cli/cli_config.h"
#include "cli/diagnostic_output.h"
#include "cli/result_code.h"
#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/io/io_util.h"
#include "path/path.h"
#include "pipeline/check.h"
#include "pipeline/pipeline_context.h"
#include "pipeline/target.h"

namespace cli {

namespace {

// The name a program arriving on a pipe is reported under. A pipe carries
// no file behind it, so this is the honest label rather than a stand-in
// for a path the user could open.
constexpr std::string_view STDIN_NAME = "<stdin>";

// Reads standard input to its end. A pipe hands over whatever is ready, so
// the read count is the loop's condition and only a negative one is an
// error.
base::Result<std::string, std::string_view> read_stdin() {
  std::string text;
  // Doubling until the end: a program is not known to be small, and one
  // read cannot say how much is left.
  constexpr usize CHUNK = static_cast<usize>(64) * 1024;
  std::vector<char> buffer(CHUNK);
  while (true) {
    const isize got = io::read(io::STDIN_FD, buffer.data(), buffer.size());
    if (got < 0) {
      return base::make_err(std::string_view("cannot read standard input"));
    }
    if (got == 0) {
      return base::make_ok(std::move(text));
    }
    text.append(buffer.data(), static_cast<usize>(got));
  }
}

void log_check_result(const pipeline::CheckResult& result) {
  base::logger.wo_prefix("checked {} file(s), {} module(s), {} function(s)",
                         result.file_count, result.module_count,
                         result.function_count);
}

}  // namespace

ResultCode run_check(const CliConfig& config,
                     const diag::RenderOptions& options) {
  pipeline::PipelineContext ctx;
  if (config.stdin_source) {
    // Reading the pipe and also naming a target is a contradiction, and
    // quietly preferring one of them would leave the user guessing which.
    if (!config.target_dir.empty()) {
      const u32 index = ctx.bag.emit(
          diag::Severity::Error, pipeline::PIPELINE_NO_MANIFEST,
          "--stdin reads the program from standard input; it takes no target, "
          "but '{}' was given",
          config.target_dir);
      (void)index;
      report_diagnostics(ctx.bag, ctx.sources, options);
      return ResultCode::CheckFailed;
    }
    base::Result<std::string, std::string_view> text = read_stdin();
    if (text.is_err()) {
      const u32 index =
          ctx.bag.emit(diag::Severity::Error, pipeline::PIPELINE_IO_ERROR,
                       "cannot read standard input");
      (void)index;
      report_diagnostics(ctx.bag, ctx.sources, options);
      return ResultCode::CheckFailed;
    }
    // The manager copies the text, so the buffer can go straight after.
    base::Result<pipeline::CheckResult, diag::Reported> result =
        pipeline::check_source(ctx, STDIN_NAME, std::move(text).unwrap());
    report_diagnostics(ctx.bag, ctx.sources, options);
    if (result.is_err()) {
      return ResultCode::CheckFailed;
    }
    log_check_result(std::move(result).unwrap());
    return ResultCode::Success;
  }

  const bool check_current_dir = config.target_dir.empty();
  const std::string_view raw_target =
      check_current_dir ? "." : config.target_dir;
  base::Result<pipeline::ManifestProbe, path::PathError> probe =
      pipeline::find_package_manifest(ctx, raw_target);
  if (probe.is_err()) {
    report_diagnostics(ctx.bag, ctx.sources, options);
    return ResultCode::CheckFailed;
  }
  pipeline::ManifestProbe found = std::move(probe).unwrap();
  if (found.found) {
    base::Result<pipeline::CheckResult, diag::Reported> result =
        pipeline::check_package(ctx, found.root, found.manifest,
                                found.manifest_name);
    report_diagnostics(ctx.bag, ctx.sources, options);
    if (result.is_err()) {
      return ResultCode::CheckFailed;
    }
    log_check_result(std::move(result).unwrap());
    return ResultCode::Success;
  }

  // Single-file mode for explicit `foo.al` targets. Directories without
  // a manifest are not checked: module structure needs declared roots.
  if (raw_target.size() < path::SOURCE_EXTENSION.size() ||
      raw_target.substr(raw_target.size() - path::SOURCE_EXTENSION.size()) !=
          path::SOURCE_EXTENSION) {
    if (check_current_dir) {
      const u32 index =
          ctx.bag.emit(diag::Severity::Error, pipeline::PIPELINE_NO_MANIFEST,
                       "no manifest found at current directory; check a file "
                       "or add alcy.toml");
      (void)index;
    } else {
      const u32 index = ctx.bag.emit(
          diag::Severity::Error, pipeline::PIPELINE_NO_MANIFEST,
          "no manifest found at '{}'; check a file or add alcy.toml",
          raw_target);
      (void)index;
    }
    report_diagnostics(ctx.bag, ctx.sources, options);
    return ResultCode::CheckFailed;
  }
  base::Result<pipeline::CheckResult, diag::Reported> result =
      pipeline::check_single_file(ctx, raw_target);
  report_diagnostics(ctx.bag, ctx.sources, options);
  if (result.is_err()) {
    return ResultCode::CheckFailed;
  }
  log_check_result(std::move(result).unwrap());
  return ResultCode::Success;
}

}  // namespace cli
