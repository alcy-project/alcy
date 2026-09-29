// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/new_command.h"

#include <string>
#include <string_view>
#include <utility>

#include "cli/cli_config.h"
#include "cli/output.h"
#include "cli/result_code.h"
#include "pipeline/new.h"
#include "pipeline/pipeline_context.h"

namespace cli {

ResultCode run_new(const CliConfig& config,
                   pipeline::PipelineContext& ctx,
                   Envelope& envelope) {
  const std::string_view target =
      config.target_dir.empty() ? "." : config.target_dir;
  pipeline::NewResult result =
      pipeline::create_new_package(ctx, target, config.vcs);
  envelope.bag = &ctx.bag;
  envelope.sources = &ctx.sources;
  if (result.is_ok() && !ctx.bag.has_errors()) {
    envelope.status = Status::Ok;
    envelope.outcome = Outcome::CreatedPackage;
    envelope.package_name = std::move(result).unwrap();
    envelope.package_dir = std::string(target);
    return ResultCode::Success;
  }
  envelope.status = Status::Error;
  return ResultCode::NewFailed;
}

}  // namespace cli
