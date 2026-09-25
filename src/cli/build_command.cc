// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/build_command.h"

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
#include "pipeline/build.h"
#include "pipeline/pipeline_context.h"
#include "pipeline/target.h"

namespace cli {

ResultCode run_build(const CliConfig& config,
                     const diag::RenderOptions& options) {
  pipeline::PipelineContext ctx;
  const std::string_view raw_dir =
      config.target_dir.empty() ? "." : config.target_dir;

  if (raw_dir.size() >= path::kSourceExtension.size() &&
      raw_dir.substr(raw_dir.size() - path::kSourceExtension.size()) ==
          path::kSourceExtension) {
    auto res = pipeline::build_single_file(ctx, raw_dir, config.output,
                                           config.release, config.linker);
    if (!res.is_ok() || ctx.bag.has_errors()) {
      report_diagnostics(ctx.bag, ctx.sources, options);
      return ResultCode::BuildFailed;
    }
    base::logger.wo_prefix("built successfully");
    return ResultCode::Success;
  }

  base::Result<pipeline::ManifestProbe, path::PathError> probe =
      pipeline::find_package_manifest(ctx, raw_dir);
  if (probe.is_err()) {
    report_diagnostics(ctx.bag, ctx.sources, options);
    return ResultCode::BuildFailed;
  }
  pipeline::ManifestProbe found = std::move(probe).unwrap();
  if (found.found) {
    auto res = pipeline::build_package(ctx, found.root, found.manifest,
                                       found.manifest_name, config.output,
                                       config.release, config.linker);
    if (!res.is_ok() || ctx.bag.has_errors()) {
      report_diagnostics(ctx.bag, ctx.sources, options);
      return ResultCode::BuildFailed;
    }
    base::logger.wo_prefix("built successfully");
    return ResultCode::Success;
  }

  // Without a manifest there is no module structure to build: report
  // the error instead of claiming a build that never ran.
  const u32 index = ctx.bag.emit(
      diag::Severity::Error, pipeline::kPipelineNoManifest,
      "no manifest found at '{}'; build a file or add alcy.toml", raw_dir);
  (void)index;
  report_diagnostics(ctx.bag, ctx.sources, options);
  return ResultCode::BuildFailed;
}

}  // namespace cli
