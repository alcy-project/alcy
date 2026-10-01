// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/validate.h"

#include "cli/cli_config.h"
#include "doctest/doctest.h"
#include "fpag/base/result.h"
#include "i18n/language.h"

namespace cli {

TEST_CASE("Config validation requires a subcommand") {
  const CliConfig config{};
  CHECK(validate_cli_config(config).is_err());
  CHECK(describe_config_error(ConfigError::MissingSubcommand,
                              i18n::Language::EnUs) == "No subcommand given");
}

TEST_CASE("Config validation rejects program arguments outside run") {
  CliConfig config{};
  config.subcommand = Subcommand::Check;
  config.program_args = {"extra"};
  CHECK(validate_cli_config(config).is_err());

  config.subcommand = Subcommand::Build;
  CHECK(validate_cli_config(config).is_err());
}

TEST_CASE("Config validation accepts run program arguments") {
  CliConfig config{};
  config.subcommand = Subcommand::Run;
  config.program_args = {"--flag", "value"};
  CHECK(validate_cli_config(config).is_ok());
}

TEST_CASE("Config validation accepts a plain command config") {
  CliConfig config{};
  config.subcommand = Subcommand::Init;
  CHECK(validate_cli_config(config).is_ok());
}

TEST_CASE("Config validation splits files from packages") {
  CliConfig build{};
  build.subcommand = Subcommand::Build;
  build.target_dir = "main.al";
  CHECK(validate_cli_config(build).is_err());

  CliConfig run{};
  run.subcommand = Subcommand::Run;
  run.target_dir = "main.al";
  CHECK(validate_cli_config(run).is_err());

  CliConfig compile{};
  compile.subcommand = Subcommand::Compile;
  CHECK(validate_cli_config(compile).is_err());

  CliConfig both{};
  both.subcommand = Subcommand::Compile;
  both.stdin_source = true;
  both.target_dir = "main.al";
  CHECK(validate_cli_config(both).is_err());

  CliConfig piped{};
  piped.subcommand = Subcommand::Compile;
  piped.stdin_source = true;
  CHECK(validate_cli_config(piped).is_err());

  CliConfig named{};
  named.subcommand = Subcommand::Compile;
  named.stdin_source = true;
  named.output = "main";
  CHECK(validate_cli_config(named).is_ok());

  CliConfig check{};
  check.subcommand = Subcommand::Check;
  check.target_dir = "pkg";
  check.file = "main.al";
  CHECK(validate_cli_config(check).is_err());
}

}  // namespace cli
