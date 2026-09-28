// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/run_command.h"

#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "cli/cli_config.h"
#include "cli/diagnostic_output.h"
#include "cli/result_code.h"
#include "cli/trace.h"
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
  TraceSession trace(ctx, config.time_trace);
  // Validation keeps single files out: `run` takes a package directory,
  // and `compile` takes the file.
  const std::string_view raw_dir =
      config.target_dir.empty() ? "." : config.target_dir;
  const std::span<const std::string_view> args(config.program_args);

  base::Result<pipeline::ManifestProbe, path::PathError> probe =
      pipeline::find_package_manifest(ctx, raw_dir);
  if (probe.is_err()) {
    report_diagnostics(ctx.bag, ctx.sources, options);
    return result_code(ResultCode::RunFailed);
  }
  pipeline::ManifestProbe found = std::move(probe).unwrap();
  if (!found.found) {
    const u32 index =
        ctx.bag.emit(diag::Severity::Error, pipeline::PIPELINE_NO_MANIFEST,
                     "no manifest found at '{}'; add alcy.toml", raw_dir);
    (void)index;
    report_diagnostics(ctx.bag, ctx.sources, options);
    return result_code(ResultCode::RunFailed);
  }
  if (config.time_trace) {
    const path::Path out_dir = found.root.join(path::DEFAULT_OUT_DIR);
    if (pipeline::ensure_directories(ctx, out_dir.as_view()).is_ok()) {
      trace.set_path(std::string(out_dir.join("trace.json").as_view()));
    }
  }
  base::Result<pipeline::RunOutcome, diag::Reported> result =
      pipeline::run_package(ctx, found.root, found.manifest,
                            found.manifest_name, config.release, config.linker,
                            args);
  if (!trace.finish()) {
    const u32 index =
        ctx.bag.emit(diag::Severity::Error, pipeline::PIPELINE_IO_ERROR,
                     "cannot write trace '{}'", trace.path());
    (void)index;
  }
  // Report before branching: a run that reaches the program still
  // carries the warnings the bag collected along the way.
  report_diagnostics(ctx.bag, ctx.sources, options);
  if (result.is_err()) {
    return result_code(ResultCode::RunFailed);
  }
  return std::move(result).unwrap().exit_code;
}

}  // namespace cli
