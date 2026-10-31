// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/fmt_command.h"

#include "cli/cli_config.h"
#include "doctest/doctest.h"

namespace cli {
TEST_CASE("fmt leaves standard input alone when it names no file") {
  CliConfig config{};
  config.subcommand = Subcommand::Fmt;
  CHECK_FALSE(reads_stdin(config));
}
TEST_CASE("fmt reads standard input when --stdin asks for it") {
  CliConfig config{};
  config.subcommand = Subcommand::Fmt;
  config.fmt_stdin = true;
  CHECK(reads_stdin(config));
}
TEST_CASE("fmt reads the named files instead of standard input") {
  CliConfig config{};
  config.subcommand = Subcommand::Fmt;
  config.fmt_files = {"main.al"};
  CHECK_FALSE(reads_stdin(config));
}
TEST_CASE("fmt reads the named files even when --stdin is present") {
  CliConfig config{};
  config.subcommand = Subcommand::Fmt;
  config.fmt_stdin = true;
  config.fmt_files = {"main.al"};
  CHECK(reads_stdin(config));
}
}  // namespace cli
