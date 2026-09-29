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
#include "diag/diagnostic.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/io/io_util.h"
#include "path/path.h"
#include "pipeline/build.h"
#include "pipeline/pipeline_context.h"
#include "pipeline/target.h"
#include "pkg/toolchain.h"

namespace cli {

namespace {

// What was written is what the reader wants to know after a build. The
// pipeline resolved the path, so the report names the file that exists
// and measures that, rather than leaving the reader to guess which of
// the requested and the written differ.
void record_output(const std::string& output, Envelope& envelope) {
  envelope.output_path = output;
  const isize size = io::file_size(output);
  if (size > 0) {
    envelope.output_bytes = static_cast<u64>(size);
  }
}

}  // namespace

ResultCode run_build(const CliConfig& config,
                     pipeline::PipelineContext& ctx,
                     Envelope& envelope) {
  TraceSession trace(ctx, config.time_trace);
  envelope.bag = &ctx.bag;
  envelope.sources = &ctx.sources;
  const ResultCode failed = ResultCode::BuildFailed;
  // Validation keeps single files out: `build` takes a package
  // directory, and `compile` takes the file.
  const bool build_current_dir = config.target_dir.empty();
  const std::string_view raw_dir = build_current_dir ? "." : config.target_dir;

  base::Result<pipeline::ManifestProbe, path::PathError> probe =
      pipeline::find_package_manifest(ctx, raw_dir);
  if (probe.is_err()) {
    return failed;
  }
  pipeline::ManifestProbe found = std::move(probe).unwrap();
  if (!found.found) {
    // Without a manifest there is no module structure to build: report
    // the error instead of claiming a build that never ran.
    if (build_current_dir) {
      const u32 index =
          ctx.bag.emit(diag::Severity::Error, pipeline::PIPELINE_NO_MANIFEST,
                       "no manifest found at current directory; add alcy.toml");
      (void)index;
    } else {
      const u32 index =
          ctx.bag.emit(diag::Severity::Error, pipeline::PIPELINE_NO_MANIFEST,
                       "no manifest found at '{}'; add alcy.toml", raw_dir);
      (void)index;
    }
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
  base::Result<std::string, diag::Reported> res = pipeline::build_package(
      ctx, found.root, found.manifest, found.manifest_name, config.output,
      config.release, linker, config.emit);
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
