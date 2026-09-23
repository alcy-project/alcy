// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <optional>
#include <string>

#include "cli/parse_args.h"
#include "cli/result_code.h"
#include "fpag/arg/parser.h"
#include "fpag/base/numeric.h"
#include "fpag/term/color_mode.h"
#include "fpag/term/color_style.h"

namespace cli {

// Best-effort `--color` scan over the raw arguments (element 0 is the
// program name). Used to style messages for outcomes that carry no config,
// such as parse errors. Unknown or absent values yield ColorMode::Auto.
term::ColorMode scan_color_mode(i32 argc, const char* const* argv);

// Renders an interruption outcome (help, version, error, ...) for display.
// Returns an empty string for CliConfig, which has nothing to display.
std::string render_outcome(const arg::Parser& parser,
                           const ParseOutcome& outcome,
                           term::ColorStyle style);

// Exit code for interruption outcomes. Returns nullopt for CliConfig,
// which the cli dispatches instead of exiting.
std::optional<ResultCode> interruption_exit_code(const ParseOutcome& outcome);

}  // namespace cli
