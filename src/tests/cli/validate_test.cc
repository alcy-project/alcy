// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/validate.h"

#include "cli/cli_config.h"
#include "doctest/doctest.h"
#include "fpag/base/result.h"

namespace cli {

TEST_CASE("Config validation requires a subcommand") {
  const CliConfig config{};
  CHECK(validate_cli_config(config).is_err());
  CHECK(describe_config_error(ConfigError::MissingSubcommand) ==
        "no subcommand given");
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

}  // namespace cli
