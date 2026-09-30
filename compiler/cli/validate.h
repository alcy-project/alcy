// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string>

#include "cli/cli_config.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "i18n/language.h"

namespace cli {

// Semantic failure of a parsed config, distinct from grammar failures:
// parse_args never produces these; they are combinations of otherwise
// valid fields that make no sense to dispatch.
enum class ConfigError : u8 {
  // Nothing to dispatch: no subcommand was selected.
  MissingSubcommand,
  // Trailing program arguments are only meaningful for `run`.
  UnexpectedProgramArgs,
  // `build` takes a package directory; a single file is `compile`'s.
  BuildSingleFile,
  // `run` takes a package directory; a single file is `compile`'s.
  RunSingleFile,
  // `compile` needs something to compile: a file or `--stdin`.
  CompileNeedsTarget,
  // Reading the pipe and also naming a target is a contradiction.
  CompileTargetWithStdin,
  // `--stdin` names no file, so the output needs a name.
  CompileStdinNeedsOutput,
  // `check` takes a directory or `--file`, not both.
  CheckFileWithTarget,
  // `--no-std` and `--deps` select the prelude of a single file, so
  // they only mean anything for `compile`.
  DepsWithoutCompile,
  // `--json` reports a result a tool reads, and only the verbs that
  // produce one accept it.
  JsonWithoutResult,
};

// Semantic validation of a parsed config, kept separate from grammar
// parsing: parse_args owns argv syntax, this owns field combinations.
// Pure: no I/O, no output; the caller renders the failure.
base::Result<void, ConfigError> validate_cli_config(const CliConfig& config);

// The detail for a config failure, in the invocation's language.
std::string describe_config_error(ConfigError error, i18n::Language language);

}  // namespace cli
