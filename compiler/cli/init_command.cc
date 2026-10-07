// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/init_command.h"

#include <string>
#include <string_view>
#include <utility>

#include "cli/cli_config.h"
#include "cli/output.h"
#include "cli/result_code.h"
#include "pipeline/new.h"
#include "pipeline/pipeline_context.h"

namespace cli {

ResultCode run_init(const CliConfig& config,
                    pipeline::PipelineContext& ctx,
                    Envelope& envelope) {
  const std::string_view target =
      config.target_dir.empty() ? "." : config.target_dir;
  const bool creates_suite = !config.suite.empty();
  pipeline::NewResult result =
      creates_suite
          ? pipeline::init_suite(ctx, target, config.suite, config.vcs)
          : pipeline::init_package(ctx, target, config.vcs);
  envelope.bag = &ctx.bag;
  envelope.sources = &ctx.sources;
  if (result.is_ok() && !ctx.bag.has_errors()) {
    const pipeline::ScaffoldResult created = std::move(result).unwrap();
    envelope.status = Status::Ok;
    envelope.outcome =
        creates_suite ? Outcome::CreatedSuite : Outcome::CreatedPackage;
    envelope.package_name = created.name;
    envelope.suite_name = created.suite;
    envelope.package_dir = std::string(target);
    return ResultCode::Success;
  }
  envelope.status = Status::Error;
  return ResultCode::NewFailed;
}

}  // namespace cli
