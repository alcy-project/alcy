// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/check_command.h"

#include <string_view>
#include <utility>

#include "base/logger.h"
#include "cli/cli_config.h"
#include "cli/diagnostic_output.h"
#include "cli/result_code.h"
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
  const std::string_view raw_target =
      config.target_dir.empty() ? "." : config.target_dir;
  base::Result<pipeline::ManifestProbe, path::PathError> probe =
      pipeline::find_package_manifest(ctx, raw_target);
  if (probe.is_err()) {
    report_diagnostics(ctx.bag, ctx.sources, options);
    return ResultCode::CheckFailed;
  }
  pipeline::ManifestProbe found = std::move(probe).unwrap();
  if (found.found) {
    const pipeline::CheckResult result = pipeline::check_package(
        ctx, found.root, found.manifest, found.manifest_name);
    report_diagnostics(ctx.bag, ctx.sources, options);
    log_check_result(result);
    return result.success ? ResultCode::Success : ResultCode::CheckFailed;
  }

  // Single-file mode for explicit `foo.al` targets. Directories without
  // a manifest are not checked: module structure needs declared roots.
  if (raw_target.size() < path::kSourceExtension.size() ||
      raw_target.substr(raw_target.size() - path::kSourceExtension.size()) !=
          path::kSourceExtension) {
    const u32 index = ctx.bag.emit(
        diag::Severity::Error, pipeline::kPipelineNoManifest,
        "no manifest found at '{}'; check a file or add alcy.toml", raw_target);
    (void)index;
    report_diagnostics(ctx.bag, ctx.sources, options);
    return ResultCode::CheckFailed;
  }
  const pipeline::CheckResult result =
      pipeline::check_single_file(ctx, raw_target);
  report_diagnostics(ctx.bag, ctx.sources, options);
  log_check_result(result);
  return result.success ? ResultCode::Success : ResultCode::CheckFailed;
}

}  // namespace cli
