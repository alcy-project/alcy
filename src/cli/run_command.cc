// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/run_command.h"

#include <span>
#include <string_view>
#include <utility>

#include "cli/cli_config.h"
#include "cli/diagnostic_output.h"
#include "cli/result_code.h"
#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "path/path.h"
#include "pipeline/pipeline_context.h"
#include "pipeline/run.h"
#include "pipeline/target.h"

namespace cli {

i32 run_run(const CliConfig& config, const diag::RenderOptions& options) {
  pipeline::PipelineContext ctx;
  const std::string_view raw_dir =
      config.target_dir.empty() ? "." : config.target_dir;
  const std::span<const std::string_view> args(config.program_args);

  if (raw_dir.size() >= path::SOURCE_EXTENSION.size() &&
      raw_dir.substr(raw_dir.size() - path::SOURCE_EXTENSION.size()) ==
          path::SOURCE_EXTENSION) {
    base::Result<pipeline::RunOutcome, diag::Reported> result =
        pipeline::run_single_file(ctx, raw_dir, config.release, config.linker,
                                  args);
    if (result.is_err()) {
      report_diagnostics(ctx.bag, ctx.sources, options);
      return result_code(ResultCode::RunFailed);
    }
    return std::move(result).unwrap().exit_code;
  }

  base::Result<pipeline::ManifestProbe, path::PathError> probe =
      pipeline::find_package_manifest(ctx, raw_dir);
  if (probe.is_err()) {
    report_diagnostics(ctx.bag, ctx.sources, options);
    return result_code(ResultCode::RunFailed);
  }
  pipeline::ManifestProbe found = std::move(probe).unwrap();
  if (!found.found) {
    const u32 index = ctx.bag.emit(
        diag::Severity::Error, pipeline::PIPELINE_NO_MANIFEST,
        "no manifest found at '{}'; run a file or add alcy.toml", raw_dir);
    (void)index;
    report_diagnostics(ctx.bag, ctx.sources, options);
    return result_code(ResultCode::RunFailed);
  }
  base::Result<pipeline::RunOutcome, diag::Reported> result =
      pipeline::run_package(ctx, found.root, found.manifest,
                            found.manifest_name, config.release, config.linker,
                            args);
  if (result.is_err()) {
    report_diagnostics(ctx.bag, ctx.sources, options);
    return result_code(ResultCode::RunFailed);
  }
  return std::move(result).unwrap().exit_code;
}

}  // namespace cli
