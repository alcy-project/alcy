// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/init_command.h"

#include <string_view>

#include "base/logger.h"
#include "cli/cli_config.h"
#include "cli/result_code.h"
#include "pipeline/new.h"
#include "pipeline/pipeline_context.h"

namespace cli {

ResultCode run_init(const CliConfig& config) {
  pipeline::PipelineContext ctx;
  const std::string_view raw_dir =
      config.target_dir.empty() ? "." : config.target_dir;
  pipeline::NewResult result = pipeline::init_package(ctx, raw_dir);
  if (result.is_ok() && !ctx.bag.has_errors()) {
    base::logger.wo_prefix("created package in '{}'", raw_dir);
    return ResultCode::Success;
  }
  pipeline::report(ctx.bag, ctx.sources);
  return ResultCode::NewFailed;
}

}  // namespace cli
