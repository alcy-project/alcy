// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "app/parse_output.h"

#include <optional>
#include <string>
#include <string_view>

#include "app/driver_config.h"
#include "app/parse_args.h"
#include "app/result_code.h"
#include "debug/fatal.h"
#include "fmt/format.h"
#include "fpag/arg/error_formatter.h"
#include "fpag/arg/help_formatter.h"
#include "fpag/arg/parser.h"
#include "fpag/arg/version_formatter.h"
#include "fpag/base/numeric.h"
#include "fpag/term/color_mode.h"
#include "fpag/term/color_style.h"

namespace app {

term::ColorMode scan_color_mode(i32 argc, const char* const* argv) {
  if (argv == nullptr) {
    return term::ColorMode::Auto;
  }
  term::ColorMode mode = term::ColorMode::Auto;
  for (i32 i = 1; i < argc; ++i) {
    if (argv[i] == nullptr) {
      continue;
    }
    const std::string_view arg(argv[i]);
    std::string_view value;
    if (arg == "--color") {
      if (i + 1 >= argc || argv[i + 1] == nullptr) {
        continue;
      }
      value = std::string_view(argv[++i]);
    } else if (arg.starts_with("--color=")) {
      value = arg.substr(sizeof("--color=") - 1);
    } else {
      continue;
    }
    if (term::str_to_color_mode(value) != term::ColorMode::Unknown) {
      mode = term::str_to_color_mode(value);
    }
  }
  return mode;
}

std::string render_outcome(const arg::Parser& parser,
                           const ParseOutcome& outcome,
                           term::ColorStyle style) {
  if (outcome.is<DriverConfig>()) {
    return "";
  }
  if (outcome.is<HelpRequested>() || outcome.is<NoSubcommand>()) {
    return parser.help_message(arg::DefaultHelpFormatter{}, style);
  }
  if (outcome.is<VersionRequested>()) {
    return parser.version_message(arg::DefaultVersionFormatter{}, style);
  }
  if (outcome.is<UnknownSubcommand>()) {
    const UnknownSubcommand& unknown = outcome.get<UnknownSubcommand>();
    return fmt::format("unknown subcommand '{}'; see '{} --help'", unknown.name,
                       parser.root_command().name());
  }
  if (outcome.is<ParseFailure>()) {
    const ParseFailure& failure = outcome.get<ParseFailure>();
    const arg::DefaultErrorFormatter format;
    return format(parser.root_command().name(), failure.errors, style);
  }
  UNREACHABLE();
}

std::optional<ResultCode> interruption_exit_code(const ParseOutcome& outcome) {
  if (outcome.is<DriverConfig>()) {
    return std::nullopt;
  }
  if (outcome.is<HelpRequested>() || outcome.is<VersionRequested>() ||
      outcome.is<NoSubcommand>()) {
    return ResultCode::Success;
  }
  return ResultCode::ArgParseError;
}

}  // namespace app
