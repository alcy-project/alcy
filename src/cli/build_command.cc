// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/build_command.h"

#include <string_view>
#include <utility>

#include "base/logger.h"
#include "cli/cli_config.h"
#include "cli/result_code.h"
#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "path/path.h"
#include "pipeline/build.h"
#include "pipeline/pipeline.h"
#include "pipeline/pipeline_context.h"
#include "pkg/manifest.h"
#include "source/source.h"

namespace cli {

ResultCode run_build(const CliConfig& config) {
  pipeline::PipelineContext ctx;
  const std::string_view raw_dir =
      config.target_dir.empty() ? "." : config.target_dir;

  if (raw_dir.size() >= path::kSourceExtension.size() &&
      raw_dir.substr(raw_dir.size() - path::kSourceExtension.size()) ==
          path::kSourceExtension) {
    auto res = pipeline::build_single_file(ctx, raw_dir, config.output,
                                           config.release);
    if (!res.is_ok() || ctx.bag.has_errors()) {
      pipeline::report(ctx.bag, ctx.sources);
      return ResultCode::BuildFailed;
    }
    base::logger.wo_prefix("built successfully");
    return ResultCode::Success;
  }

  base::Result<path::Path, path::PathError> dir =
      path::Path::from_native(raw_dir);
  if (dir.is_err()) {
    const u32 index = ctx.bag.emit(diag::Severity::Error, 3001,
                                   "invalid target directory '{}'", raw_dir);
    (void)index;
    pipeline::report(ctx.bag, ctx.sources);
    return ResultCode::BuildFailed;
  }
  const path::Path root = std::move(dir).unwrap();
  const path::Path manifest_path = root.join(pkg::kManifestFileName);
  base::Result<source::FileId, source::SourceError> manifest =
      ctx.sources.load(manifest_path.as_view());
  if (manifest.is_ok()) {
    auto res = pipeline::build_package(ctx, root, std::move(manifest).unwrap(),
                                       manifest_path.as_view(), config.output,
                                       config.release);
    if (!res.is_ok() || ctx.bag.has_errors()) {
      pipeline::report(ctx.bag, ctx.sources);
      return ResultCode::BuildFailed;
    }
    base::logger.wo_prefix("built successfully");
    return ResultCode::Success;
  }

  const u32 index = ctx.bag.emit(diag::Severity::Warning, 3000,
                                 "no manifest found at '{}'", raw_dir);
  (void)index;
  diag::Fallible<pipeline::DiscoveredSources> discovered =
      pipeline::discover_sources(root.as_view(), ctx.sources, ctx.bag);

  pipeline::report(ctx.bag, ctx.sources);
  if (discovered.is_err() || ctx.bag.has_errors()) {
    return ResultCode::BuildFailed;
  }
  base::logger.wo_prefix("built {} file(s)",
                         std::move(discovered).unwrap().files.size());
  return ResultCode::Success;
}

}  // namespace cli
