// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/new_command.h"

#include <string_view>

#include "cli/diagnostic_output.h"
#include "cli/result_code.h"
#include "pipeline/new.h"
#include "pipeline/pipeline_context.h"

namespace cli {

ResultCode run_new(std::string_view target_dir,
                   const diag::RenderOptions& options) {
  pipeline::PipelineContext ctx;
  pipeline::NewResult result = pipeline::create_new_package(ctx, target_dir);
  if (result.is_ok() && !ctx.bag.has_errors()) {
    return ResultCode::Success;
  }
  report_diagnostics(ctx.bag, ctx.sources, options);
  return ResultCode::NewFailed;
}

}  // namespace cli
