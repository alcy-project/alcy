// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/validate.h"

#include <string>
#include <string_view>

#include "cli/cli_config.h"
#include "fpag/base/result.h"
#include "i18n/language.h"
#include "i18n/messages.h"
#include "path/path.h"

namespace cli {

base::Result<void, ConfigError> validate_cli_config(const CliConfig& config) {
  if (config.subcommand == Subcommand::None) {
    return base::make_err(ConfigError::MissingSubcommand);
  }
  if (config.release && config.profile_given) {
    return base::make_err(ConfigError::ReleaseWithProfile);
  }
  if (!config.program_args.empty() && config.subcommand != Subcommand::Run) {
    return base::make_err(ConfigError::UnexpectedProgramArgs);
  }
  if (config.subcommand == Subcommand::Build &&
      path::has_source_extension(config.input)) {
    return base::make_err(ConfigError::BuildSingleFile);
  }
  if (config.subcommand == Subcommand::Run &&
      path::has_source_extension(config.input)) {
    return base::make_err(ConfigError::RunSingleFile);
  }
  if (config.subcommand == Subcommand::Compile) {
    if (config.stdin_source && !config.input.empty()) {
      return base::make_err(ConfigError::CompileInputWithStdin);
    }
    if (config.stdin_source && config.output.empty()) {
      return base::make_err(ConfigError::CompileStdinNeedsOutput);
    }
    if (!config.stdin_source && config.input.empty()) {
      return base::make_err(ConfigError::CompileNeedsInput);
    }
  }
  if ((config.subcommand == Subcommand::Check ||
       config.subcommand == Subcommand::Lint) &&
      !config.file.empty() && !config.input.empty()) {
    return base::make_err(ConfigError::CheckFileWithInput);
  }
  if (config.subcommand != Subcommand::Compile &&
      (config.no_std || !config.deps.empty())) {
    return base::make_err(ConfigError::DepsWithoutCompile);
  }
  if (config.json && config.subcommand != Subcommand::Build &&
      config.subcommand != Subcommand::Compile &&
      config.subcommand != Subcommand::Check &&
      config.subcommand != Subcommand::Lint) {
    return base::make_err(ConfigError::JsonWithoutResult);
  }
  return base::make_ok();
}

std::string describe_config_error(ConfigError error, i18n::Language language) {
  using i18n::Key;
  switch (error) {
    case ConfigError::MissingSubcommand:
      return i18n::format<Key::CliNoSubcommand>(language);
    case ConfigError::UnexpectedProgramArgs:
      return i18n::format<Key::CliProgramArgsOnlyForRun>(language);
    case ConfigError::BuildSingleFile:
      return i18n::format<Key::CliBuildTakesDirectory>(language);
    case ConfigError::RunSingleFile:
      return i18n::format<Key::CliRunTakesDirectory>(language);
    case ConfigError::CompileNeedsInput:
      return i18n::format<Key::CliCompileNeedsInput>(language);
    case ConfigError::CompileInputWithStdin:
      return i18n::format<Key::CliStdinTakesNoInput>(language);
    case ConfigError::CompileStdinNeedsOutput:
      return i18n::format<Key::CliStdinNeedsOutput>(language);
    case ConfigError::CheckFileWithInput:
      return i18n::format<Key::CliCheckFileOrInput>(language);
    case ConfigError::DepsWithoutCompile:
      return i18n::format<Key::CliDepsOnlyForCompile>(language);
    case ConfigError::JsonWithoutResult:
      return i18n::format<Key::CliJsonOnlyForResultVerbs>(language);
    case ConfigError::ReleaseWithProfile:
      return i18n::format<Key::CliReleaseWithProfile>(language);
  }
  return i18n::format<Key::CliInvalidConfiguration>(language);
}

}  // namespace cli
