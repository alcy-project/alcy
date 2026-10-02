// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/build_command.h"

#include <string>
#include <string_view>
#include <utility>

#include "cli/cli_config.h"
#include "cli/output.h"
#include "cli/result_code.h"
#include "cli/trace.h"
#include "diag/bag.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/io/io_util.h"
#include "pipeline/build.h"
#include "pipeline/link_options.h"
#include "pipeline/pipeline_context.h"
#include "pipeline/target.h"
#include "pkg/toolchain.h"

namespace cli {

ResultCode run_build(const CliConfig& config,
                     pipeline::PipelineContext& ctx,
                     Envelope& envelope) {
  TraceSession trace(ctx, config.time_trace);
  envelope.bag = &ctx.bag;
  envelope.sources = &ctx.sources;
  const ResultCode failed = ResultCode::BuildFailed;
  // Validation keeps single files out: `build` takes a package
  // directory, and `compile` takes the file.
  const std::string_view raw_dir =
      config.target_dir.empty() ? "." : config.target_dir;
  base::Result<pipeline::ManifestProbe, diag::Reported> probe =
      pipeline::require_package_manifest(ctx, raw_dir, false);
  if (probe.is_err()) {
    return failed;
  }
  pipeline::ManifestProbe found = std::move(probe).unwrap();

  base::Result<pkg::Toolchain, diag::Reported> toolchain =
      pipeline::load_toolchain(ctx, found.root);
  if (toolchain.is_err()) {
    return failed;
  }
  const pkg::Toolchain tool = std::move(toolchain).unwrap();
  const pipeline::LinkOptions link = resolve_link_options(config, tool);
  base::Result<std::string, diag::Reported> res = pipeline::build_package(
      ctx, found.root, found.manifest, found.manifest_name, config.output,
      config.release, link, config.emit);
  envelope.trace = trace.take_events();
  if (res.is_err() || ctx.bag.has_errors()) {
    return failed;
  }
  record_output(std::move(res).unwrap(), envelope);
  envelope.status = Status::Ok;
  envelope.outcome = Outcome::Built;
  return ResultCode::Success;
}

}  // namespace cli
