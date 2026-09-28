// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/check_command.h"

#include <string_view>
#include <utility>

#include "base/logger.h"
#include "cli/cli_config.h"
#include "cli/diagnostic_output.h"
#include "cli/result_code.h"
#include "cli/trace.h"
#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "path/path.h"
#include "pipeline/check.h"
#include "pipeline/pipeline_context.h"
#include "pipeline/target.h"

namespace cli {

namespace {

void log_check_result(const pipeline::CheckResult& result) {
  base::logger.wo_prefix("checked {} file(s), {} module(s), {} function(s)",
                         result.file_count, result.module_count,
                         result.function_count);
}

}  // namespace

ResultCode run_check(const CliConfig& config,
                     const diag::RenderOptions& options) {
  pipeline::PipelineContext ctx;
  // The trace lands in the working directory: check writes nothing, so
  // there is no output to sit beside.
  TraceSession trace(ctx, config.time_trace);

  // Validation keeps the two forms apart: `--file` names one source,
  // and the positional names a package directory.
  if (!config.file.empty()) {
    base::Result<pipeline::CheckResult, diag::Reported> result =
        pipeline::check_single_file(ctx, config.file);
    if (!trace.finish()) {
      const u32 index =
          ctx.bag.emit(diag::Severity::Error, pipeline::PIPELINE_IO_ERROR,
                       "cannot write trace '{}'", trace.path());
      (void)index;
    }
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
    if (!trace.finish()) {
      const u32 index =
          ctx.bag.emit(diag::Severity::Error, pipeline::PIPELINE_IO_ERROR,
                       "cannot write trace '{}'", trace.path());
      (void)index;
    }
    report_diagnostics(ctx.bag, ctx.sources, options);
    if (result.is_err()) {
      return ResultCode::CheckFailed;
    }
    log_check_result(std::move(result).unwrap());
    return ResultCode::Success;
  }

  // Directories without a manifest are not checked: module structure
  // needs declared roots.
  if (check_current_dir) {
    const u32 index =
        ctx.bag.emit(diag::Severity::Error, pipeline::PIPELINE_NO_MANIFEST,
                     "no manifest found at current directory; pass a file "
                     "with --file or add alcy.toml");
    (void)index;
  } else {
    const u32 index = ctx.bag.emit(
        diag::Severity::Error, pipeline::PIPELINE_NO_MANIFEST,
        "no manifest found at '{}'; pass a file with --file or add "
        "alcy.toml",
        raw_target);
    (void)index;
  }
  report_diagnostics(ctx.bag, ctx.sources, options);
  return ResultCode::CheckFailed;
}

}  // namespace cli
