// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/new_command.h"

#include <string_view>

#include "cli/result_code.h"
#include "pipeline/new.h"
#include "pipeline/pipeline_context.h"

namespace cli {

ResultCode run_new(std::string_view target_dir) {
  pipeline::PipelineContext ctx;
  pipeline::NewResult result = pipeline::create_new_package(ctx, target_dir);
  if (result.is_ok()) {
    return ResultCode::Success;
  } else {
    pipeline::report(ctx.bag, ctx.sources);
    return ResultCode::NewFailed;
  }
}

}  // namespace cli
