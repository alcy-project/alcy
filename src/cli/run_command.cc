// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/run_command.h"

#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "cli/cli_config.h"
#include "cli/output.h"
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
#include "pkg/toolchain.h"

namespace cli {

i32 run_run(const CliConfig& config,
            pipeline::PipelineContext& ctx,
            Envelope& envelope) {
  TraceSession trace(ctx, config.time_trace);
  envelope.bag = &ctx.bag;
  envelope.sources = &ctx.sources;
  const i32 failed = result_code(ResultCode::RunFailed);
  // Validation keeps single files out: `run` takes a package directory,
  // and `compile` takes the file.
  const std::string_view raw_dir =
      config.target_dir.empty() ? "." : config.target_dir;
  const std::span<const std::string_view> args(config.program_args);

  base::Result<pipeline::ManifestProbe, path::PathError> probe =
      pipeline::find_package_manifest(ctx, raw_dir);
  if (probe.is_err()) {
    return failed;
  }
  pipeline::ManifestProbe found = std::move(probe).unwrap();
  if (!found.found) {
    const u32 index =
        ctx.bag.emit(diag::Severity::Error, pipeline::PIPELINE_NO_MANIFEST,
                     "no manifest found at '{}'; add alcy.toml", raw_dir);
    (void)index;
    return failed;
  }
  base::Result<pkg::Toolchain, diag::Reported> toolchain =
      pipeline::load_toolchain(ctx, found.root);
  if (toolchain.is_err()) {
    return failed;
  }
  // An explicit driver wins; the file names the default.
  const pkg::Toolchain tool = std::move(toolchain).unwrap();
  const std::string_view linker =
      config.linker.empty() ? tool.linker : config.linker;
  base::Result<pipeline::RunOutcome, diag::Reported> result =
      pipeline::run_package(ctx, found.root, found.manifest,
                            found.manifest_name, config.release, linker, args);
  envelope.trace = trace.take_events();
  if (result.is_err()) {
    return failed;
  }
  // A run that reaches the program still carries the warnings the bag
  // collected along the way, so the report is not a success-only line.
  envelope.status = Status::Ok;
  envelope.summary = "ran";
  return std::move(result).unwrap().exit_code;
}

}  // namespace cli
