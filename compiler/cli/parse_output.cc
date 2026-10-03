// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/parse_output.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "cli/cli_config.h"
#include "cli/parse_args.h"
#include "cli/result_code.h"
#include "cli/usage.h"
#include "debug/fatal.h"
#include "diag/diagnostic.h"
#include "diag/render.h"
#include "fpag/arg/parser.h"
#include "fpag/arg/version_formatter.h"
#include "fpag/base/numeric.h"
#include "fpag/term/color_mode.h"
#include "fpag/term/color_style.h"
#include "fpag/term/console.h"
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

Interruption render_outcome(const arg::Parser& parser,
                            const ParseOutcome& outcome,
                            term::ColorMode color_mode,
                            i18n::Language language) {
  if (outcome.is<CliConfig>()) {
    return {};
  }
  const HelpFormatter help{language};
  if (outcome.is<HelpRequested>() || outcome.is<NoSubcommand>()) {
    return Interruption{
        .text = parser.help_message(
            help, term::console_color_style(term::Stream::Stdout, color_mode)),
        .errors = {},
    };
  }
  if (outcome.is<VersionRequested>()) {
    // The parser's formatter writes the sentence; the newline that ends
    // the block is this layer's, because this is the layer that hands the
    // block to a writer.
    std::string text = parser.version_message(
        arg::DefaultVersionFormatter{},
        term::console_color_style(term::Stream::Stdout, color_mode));
    text += '\n';
    return Interruption{.text = std::move(text), .errors = {}};
  }
  // An error lands on standard error, so it is styled by what standard
  // error can do: `alcy --help > /dev/null` on a pipe must not decide
  // whether the answer is readable.
  const diag::RenderOptions error{
      .color = term::console_color_style(term::Stream::Stderr, color_mode) !=
               term::ColorStyle::Off,
      .language = language,
  };
  if (outcome.is<UnknownSubcommand>()) {
    const UnknownSubcommand& unknown = outcome.get<UnknownSubcommand>();
    std::string message = i18n::format<i18n::Key::CliUnknownSubcommand>(
        language, unknown.name, parser.root_command().name());
    if (!unknown.suggestion.empty()) {
      message = i18n::format<i18n::Key::CliDidYouMean>(
          language, std::move(message), unknown.suggestion);
    }
    return Interruption{
        .text = {},
        .errors = {diag::render(diag::message(diag::Severity::Error, message),
                                error)}};
  }
  if (outcome.is<ParseFailure>()) {
    const ParseFailure& failure = outcome.get<ParseFailure>();
    return Interruption{
        .text = {},
        .errors = render_parse_errors(
            parser.root_command().name(), failure.errors, failure.suggestions,
            term::console_color_style(term::Stream::Stderr, color_mode),
            language)};
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
