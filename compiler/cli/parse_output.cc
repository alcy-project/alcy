// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/parse_output.h"

#include <optional>
#include <string>
#include <string_view>

#include "cli/cli_config.h"
#include "cli/parse_args.h"
#include "cli/result_code.h"
#include "cli/usage.h"
#include "debug/fatal.h"
#include "fmt/format.h"
#include "fpag/arg/error_formatter.h"
#include "fpag/arg/help_formatter.h"
#include "fpag/arg/parser.h"
#include "fpag/arg/version_formatter.h"
#include "fpag/base/numeric.h"
#include "fpag/term/color_mode.h"
#include "fpag/term/color_style.h"
#include "i18n/language.h"
#include "i18n/messages.h"

namespace cli {

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

i18n::Language scan_language(i32 argc, const char* const* argv) {
  i18n::Language language = i18n::Language::EnUs;
  if (argv == nullptr) {
    return language;
  }
  for (i32 i = 1; i < argc; ++i) {
    if (argv[i] == nullptr) {
      continue;
    }
    const std::string_view arg(argv[i]);
    std::string_view value;
    if (arg == "--lang") {
      if (i + 1 >= argc || argv[i + 1] == nullptr) {
        continue;
      }
      value = std::string_view(argv[++i]);
    } else if (arg.starts_with("--lang=")) {
      value = arg.substr(sizeof("--lang=") - 1);
    } else {
      continue;
    }
    if (const std::optional<i18n::Language> found =
            i18n::language_from_tag(value);
        found.has_value()) {
      language = *found;
    }
  }
  return language;
}

std::string render_outcome(const arg::Parser& parser,
                           const ParseOutcome& outcome,
                           term::ColorStyle style,
                           i18n::Language language) {
  if (outcome.is<CliConfig>()) {
    return "";
  }
  const HelpFormatter help{language};
  if (outcome.is<HelpRequested>() || outcome.is<NoSubcommand>()) {
    return parser.help_message(help, style);
  }
  if (outcome.is<VersionRequested>()) {
    return parser.version_message(arg::DefaultVersionFormatter{}, style);
  }
  if (outcome.is<UnknownSubcommand>()) {
    const UnknownSubcommand& unknown = outcome.get<UnknownSubcommand>();
    std::string text = i18n::format<i18n::Key::CliUnknownSubcommand>(
        language, unknown.name, parser.root_command().name());
    if (!unknown.suggestion.empty()) {
      text += '\n';
      text +=
          i18n::format<i18n::Key::CliDidYouMean>(language, unknown.suggestion);
    }
    return text;
  }
  if (outcome.is<ParseFailure>()) {
    const ParseFailure& failure = outcome.get<ParseFailure>();
    const ErrorFormatter format{language};
    return format(parser.root_command().name(), failure.errors,
                  failure.suggestions, style);
  }
  UNREACHABLE();
}

std::optional<ResultCode> interruption_exit_code(const ParseOutcome& outcome) {
  if (outcome.is<CliConfig>()) {
    return std::nullopt;
  }
  if (outcome.is<HelpRequested>() || outcome.is<VersionRequested>() ||
      outcome.is<NoSubcommand>()) {
    return ResultCode::Success;
  }
  return ResultCode::ArgParseError;
}

}  // namespace cli
