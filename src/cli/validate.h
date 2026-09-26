// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string_view>

#include "cli/cli_config.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"

namespace cli {

// Semantic failure of a parsed config, distinct from grammar failures:
// parse_args never produces these; they are combinations of otherwise
// valid fields that make no sense to dispatch.
enum class ConfigError : u8 {
  // Nothing to dispatch: no subcommand was selected.
  MissingSubcommand,
  // Trailing program arguments are only meaningful for `run`.
  UnexpectedProgramArgs,
};

// Semantic validation of a parsed config, kept separate from grammar
// parsing: parse_args owns argv syntax, this owns field combinations.
// Pure: no I/O, no output; the caller renders the failure.
base::Result<void, ConfigError> validate_cli_config(const CliConfig& config);

// Short human-readable detail for a config failure.
std::string_view describe_config_error(ConfigError error);

}  // namespace cli
