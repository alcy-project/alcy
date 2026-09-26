// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/validate.h"

#include <string_view>

#include "cli/cli_config.h"
#include "fpag/base/result.h"

namespace cli {

base::Result<void, ConfigError> validate_cli_config(const CliConfig& config) {
  if (config.subcommand == Subcommand::None) {
    return base::make_err(ConfigError::MissingSubcommand);
  }
  if (!config.program_args.empty() && config.subcommand != Subcommand::Run) {
    return base::make_err(ConfigError::UnexpectedProgramArgs);
  }
  return base::make_ok();
}

std::string_view describe_config_error(ConfigError error) {
  switch (error) {
    case ConfigError::MissingSubcommand: return "no subcommand given";
    case ConfigError::UnexpectedProgramArgs:
      return "program arguments are only accepted by `run`";
  }
  return "invalid configuration";
}

}  // namespace cli
