// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "cli/parse_args.h"
#include "cli/result_code.h"
#include "fpag/arg/parser.h"
#include "fpag/base/numeric.h"
#include "fpag/term/color_mode.h"
#include "fpag/term/color_style.h"
#include "i18n/language.h"

namespace cli {

// Best-effort `--color` scan over the raw arguments (element 0 is the
// program name). Used to style messages for outcomes that carry no config,
// such as parse errors. Unknown or absent values yield ColorMode::Auto.
term::ColorMode scan_color_mode(i32 argc, const char* const* argv);

// Best-effort `--lang` scan, for the same reason: the help text is built
// before the arguments are parsed, and it has to be written in the
// language the invocation asked for. A value that names no catalog leaves
// the default, and the parser reports the value as an invalid choice
// rather than this scan falling back to it silently.
i18n::Language scan_language(i32 argc, const char* const* argv);

// What an interruption has to say, split by where it belongs. `--help`
// and `--version` answer a question, so they go to standard output; a
// parse error or an unknown verb is an error, so it goes to standard
// error. A successful parse interrupts nothing and carries neither.
//
// The errors are already the blocks `diag::render` makes of them, because
// a diagnostic views the text the catalog formats on demand and the view
// has to become a block before that text goes away.
struct Interruption {
  // The answer, as one block. Empty when the interruption is an error.
  std::string text;
  // The errors and any advice, one block each, in reading order. Empty
  // when the interruption is an answer.
  std::vector<std::string> errors;
};

// Renders an interruption outcome: the answer a help or version request
// wants, or the errors a rejected command line produced. Carries neither
// for CliConfig, which has nothing to report.
Interruption render_outcome(const arg::Parser& parser,
                            const ParseOutcome& outcome,
                            term::ColorMode color_mode,
                            i18n::Language language = i18n::Language::EnUs);

// Exit code for interruption outcomes. Returns nullopt for CliConfig,
// which the cli dispatches instead of exiting.
std::optional<ResultCode> interruption_exit_code(const ParseOutcome& outcome);

}  // namespace cli
