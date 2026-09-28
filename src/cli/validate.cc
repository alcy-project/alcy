// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/validate.h"

#include <string_view>

#include "cli/cli_config.h"
#include "fpag/base/result.h"
#include "path/path.h"

namespace cli {

namespace {

bool is_source_file(std::string_view target) {
  return target.size() >= path::SOURCE_EXTENSION.size() &&
         target.substr(target.size() - path::SOURCE_EXTENSION.size()) ==
             path::SOURCE_EXTENSION;
}

}  // namespace

base::Result<void, ConfigError> validate_cli_config(const CliConfig& config) {
  if (config.subcommand == Subcommand::None) {
    return base::make_err(ConfigError::MissingSubcommand);
  }
  if (!config.program_args.empty() && config.subcommand != Subcommand::Run) {
    return base::make_err(ConfigError::UnexpectedProgramArgs);
  }
  if (config.subcommand == Subcommand::Build &&
      is_source_file(config.target_dir)) {
    return base::make_err(ConfigError::BuildSingleFile);
  }
  if (config.subcommand == Subcommand::Run &&
      is_source_file(config.target_dir)) {
    return base::make_err(ConfigError::RunSingleFile);
  }
  if (config.subcommand == Subcommand::Compile) {
    if (config.stdin_source && !config.target_dir.empty()) {
      return base::make_err(ConfigError::CompileTargetWithStdin);
    }
    if (config.stdin_source && config.output.empty()) {
      return base::make_err(ConfigError::CompileStdinNeedsOutput);
    }
    if (!config.stdin_source && config.target_dir.empty()) {
      return base::make_err(ConfigError::CompileNeedsTarget);
    }
  }
  if (config.subcommand == Subcommand::Check && !config.file.empty() &&
      !config.target_dir.empty()) {
    return base::make_err(ConfigError::CheckFileWithTarget);
  }
  return base::make_ok();
}

std::string_view describe_config_error(ConfigError error) {
  switch (error) {
    case ConfigError::MissingSubcommand: return "no subcommand given";
    case ConfigError::UnexpectedProgramArgs:
      return "program arguments are only accepted by `run`";
    case ConfigError::BuildSingleFile:
      return "`build` takes a package directory; use `compile` for a file";
    case ConfigError::RunSingleFile:
      return "`run` takes a package directory; use `compile` for a file";
    case ConfigError::CompileNeedsTarget:
      return "`compile` needs a file target or `--stdin`";
    case ConfigError::CompileTargetWithStdin:
      return "`--stdin` takes no target";
    case ConfigError::CompileStdinNeedsOutput:
      return "`compile --stdin` needs `-o` to name the output";
    case ConfigError::CheckFileWithTarget:
      return "`check` takes a directory or `--file`, not both";
  }
  return "invalid configuration";
}

}  // namespace cli
