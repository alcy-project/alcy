// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/usage.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cli/suggest.h"
#include "debug/fatal.h"
#include "diag/diagnostic.h"
#include "diag/render.h"
#include "fmt/base.h"
#include "fmt/core.h"
#include "fmt/format.h"
#include "fpag/arg/arg.h"
#include "fpag/arg/command.h"
#include "fpag/arg/error_code.h"
#include "fpag/arg/parse_error.h"
#include "fpag/base/numeric.h"
#include "fpag/term/color_style.h"
#include "fpag/term/style.h"
#include "i18n/language.h"
#include "i18n/messages.h"

namespace cli {

namespace {

using i18n::Key;
using i18n::Language;

// The option column of one flag: "  -o, --output <value>", or the
// choices the parser will accept in place of a value name.
std::string format_option_spec(const arg::Arg& argument, Language language) {
  std::string spec;
  spec.reserve(64);

  spec += "  ";
  if (argument.short_name()) {
    spec += '-';
    spec += *argument.short_name();
    if (!argument.long_name().empty()) {
      spec += ", ";
    }
  } else {
    spec += "    ";
  }

  if (!argument.long_name().empty()) {
    spec += "--";
    spec += argument.long_name();
  }

  if (!argument.is_flag()) {
    spec += " <";
    if (!argument.choices().empty()) {
      for (usize i = 0; i < argument.choices().size(); ++i) {
        if (i > 0) {
          spec += '|';
        }
        spec += argument.choices()[i];
      }
    } else if (!argument.value_name().empty()) {
      spec += argument.value_name();
    } else {
      spec += i18n::text<Key::ArgValueName>(language);
    }
    spec += '>';
  }

  return spec;
}

void render_option_line(std::back_insert_iterator<std::string> out,
                        std::string_view option_spec,
                        usize widest,
                        std::string_view help_text,
                        std::string_view default_value,
                        bool is_required,
                        term::ColorStyle color_style,
                        Language language) {
  const char* italic = term::style_code(term::ITALIC, color_style);
  const char* reset = term::style_code(term::RESET, color_style);
  const char* bright_cyan = term::style_code(term::FG_BRIGHT_CYAN, color_style);
  const char* gray = term::style_code(term::FG_GRAY, color_style);

  // The width of the option column is a byte count, which is what a
  // flag spec is: every character in one is ASCII.
  const usize width = option_spec.size();
  fmt::format_to(out, "{}{}{}", bright_cyan, option_spec, reset);
  if (width < widest) {
    fmt::format_to(out, "{:>{}}", "", widest - width);
  }
  fmt::format_to(out, "  ");
  fmt::format_to(out, "{}", help_text);
  if (!default_value.empty()) {
    fmt::format_to(out, " {}",
                   i18n::format<Key::ArgDefault>(language, default_value));
  }
  if (is_required) {
    fmt::format_to(out, " {}{}{}{}", gray, italic,
                   i18n::text<Key::ArgRequired>(language), reset);
  }
  fmt::format_to(out, "\n");
}

}  // namespace

UsageText::UsageText(Language language)
    : texts_(static_cast<usize>(i18n::KEY_COUNT)) {
  set(Key::CliAbout, i18n::format<Key::CliAbout>(language));
  set(Key::CliBuildAbout, i18n::format<Key::CliBuildAbout>(language));
  set(Key::CliColorHelp, i18n::format<Key::CliColorHelp>(language));
  set(Key::CliLangHelp, i18n::format<Key::CliLangHelp>(language));
  set(Key::CliTimeTraceHelp, i18n::format<Key::CliTimeTraceHelp>(language));
  set(Key::CliJsonHelp, i18n::format<Key::CliJsonHelp>(language));
  set(Key::CliReleaseHelp, i18n::format<Key::CliReleaseHelp>(language));
  set(Key::CliBuildOutputHelp, i18n::format<Key::CliBuildOutputHelp>(language));
  set(Key::CliEmitHelp, i18n::format<Key::CliEmitHelp>(language));
  set(Key::CliLinkerOverrideHelp,
      i18n::format<Key::CliLinkerOverrideHelp>(language));
  set(Key::CliLinkArgsOverrideHelp,
      i18n::format<Key::CliLinkArgsOverrideHelp>(language));
  set(Key::CliCompileAbout, i18n::format<Key::CliCompileAbout>(language));
  set(Key::CliCompileOutputHelp,
      i18n::format<Key::CliCompileOutputHelp>(language));
  set(Key::CliCompileLinkerHelp,
      i18n::format<Key::CliCompileLinkerHelp>(language));
  set(Key::CliCompileLinkArgsHelp,
      i18n::format<Key::CliCompileLinkArgsHelp>(language));
  set(Key::CliStdinHelp, i18n::format<Key::CliStdinHelp>(language));
  set(Key::CliNoStdHelp, i18n::format<Key::CliNoStdHelp>(language));
  set(Key::CliDepsHelp, i18n::format<Key::CliDepsHelp>(language));
  set(Key::CliRunAbout, i18n::format<Key::CliRunAbout>(language));
  set(Key::CliNewAbout, i18n::format<Key::CliNewAbout>(language));
  set(Key::CliInitAbout, i18n::format<Key::CliInitAbout>(language));
  set(Key::CliVcsHelp, i18n::format<Key::CliVcsHelp>(language));
  set(Key::CliCheckAbout, i18n::format<Key::CliCheckAbout>(language));
  set(Key::CliCheckFileHelp, i18n::format<Key::CliCheckFileHelp>(language));
}

const UsageText& usage_text(Language language) {
  static const std::array<UsageText, i18n::LANGUAGE_COUNT> TABLES = {
      UsageText{Language::EnUs},
  };
  return TABLES[static_cast<usize>(language)];
}

std::string HelpFormatter::operator()(const arg::Command& command,
                                      term::ColorStyle color_style) const {
  std::string result;

  constexpr usize MARGIN = 512;
  constexpr usize ESTIMATED_STR_LEN_PER_ARGS = 128;
  result.reserve(MARGIN + command.args().size() * ESTIMATED_STR_LEN_PER_ARGS);

  auto out = std::back_inserter(result);

  constexpr usize TERMINAL_WIDTH = 60;

  const char* bold = term::style_code(term::BOLD, color_style);
  const char* underline = term::style_code(term::UNDERLINE, color_style);
  const char* reset = term::style_code(term::RESET, color_style);
  const char* blue = term::style_code(term::FG_BLUE, color_style);
  const char* bright_magenta =
      term::style_code(term::FG_BRIGHT_MAGENTA, color_style);

  if (!command.name().empty()) {
    const usize pad = (command.name().size() < TERMINAL_WIDTH)
                          ? (TERMINAL_WIDTH - command.name().size()) / 2
                          : 0;
    fmt::format_to(out, "{:>{}}{}{}{}{}\n\n", "", pad, bold, underline,
                   command.name(), reset);
  }

  if (!command.about().empty()) {
    const std::string_view about = command.about();
    const usize pad = (about.size() < TERMINAL_WIDTH)
                          ? (TERMINAL_WIDTH - about.size()) / 2
                          : 0;
    fmt::format_to(out, "{:>{}}{}{}{}\n\n", "", pad, blue, about, reset);
  }

  constexpr usize MIN_DESCRIPTION_MARGIN = 20;
  if (!command.subcommands().empty()) {
    fmt::format_to(out, "{}{}{}{}: {}{}{} {}{}[{}]{} {}{}[{}]{}\n", bold,
                   underline, i18n::text<Key::ArgUsage>(language), reset, bold,
                   command.name(), reset, bright_magenta, bold,
                   i18n::text<Key::ArgOptions>(language), reset, bright_magenta,
                   bold, i18n::text<Key::ArgCommand>(language), reset);
    fmt::format_to(out, "\n{}{}{}{}:\n", bold, underline,
                   i18n::text<Key::ArgCommands>(language), reset);

    usize widest_command = MIN_DESCRIPTION_MARGIN;
    for (const arg::Command& sub : command.subcommands()) {
      widest_command = std::max(widest_command, sub.name().length());
    }
    for (const arg::Command& sub : command.subcommands()) {
      fmt::format_to(out, "  {:<{}}  {}\n", sub.name(), widest_command,
                     sub.about());
    }
  } else {
    fmt::format_to(out, "{}{}{}{}: {}{}{} {}{}[{}]{}\n", bold, underline,
                   i18n::text<Key::ArgUsage>(language), reset, bold,
                   command.name(), reset, bright_magenta, bold,
                   i18n::text<Key::ArgOptions>(language), reset);
  }

  fmt::format_to(out, "\n{}{}{}{}:\n", bold, underline,
                 i18n::text<Key::ArgOptions>(language), reset);

  usize widest_option = MIN_DESCRIPTION_MARGIN;
  std::vector<std::string> specs;
  specs.reserve(command.args().size());
  for (const arg::Arg& argument : command.args()) {
    specs.push_back(format_option_spec(argument, language));
    widest_option = std::max(widest_option, specs.back().size());
  }

  for (usize i = 0; i < command.args().size(); ++i) {
    const arg::Arg& argument = command.args()[i];
    render_option_line(out, specs[i], widest_option, argument.help(),
                       argument.default_value(), argument.is_required(),
                       color_style, language);
  }

  if (command.builtin_enabled()) {
    render_option_line(out, "  -h, --help", widest_option,
                       i18n::text<Key::ArgHelpOption>(language), "", false,
                       color_style, language);
    if (!command.version().empty()) {
      render_option_line(out, "  -v, --version", widest_option,
                         i18n::text<Key::ArgVersionOption>(language), "", false,
                         color_style, language);
    }
  }

  return result;
}

namespace {

// The message for one parse error, with the flag and the value the
// parser rejected. Each code is one message, so the catalog decides which
// arguments it takes and the build checks that the two agree.
std::string parse_error_message(const arg::ParseError& error,
                                i18n::Language language) {
  using i18n::Key;
  switch (error.code) {
    case arg::ErrorCode::InvalidArgCount:
      return i18n::format<Key::ArgInvalidArgCount>(language, error.context,
                                                   error.value);
    case arg::ErrorCode::NullMatchesPointer:
      return i18n::format<Key::ArgNullMatchesPointer>(language);
    case arg::ErrorCode::UnknownLongOption:
      return i18n::format<Key::ArgUnknownLongOption>(language, error.context);
    case arg::ErrorCode::UnknownShortOption:
      return i18n::format<Key::ArgUnknownShortOption>(language, error.context);
    case arg::ErrorCode::MissingValueForOption:
      return i18n::format<Key::ArgMissingValueForOption>(language,
                                                         error.context);
    case arg::ErrorCode::FlagTakesNoValue:
      return i18n::format<Key::ArgFlagTakesNoValue>(language, error.context);
    case arg::ErrorCode::MissingRequiredArgument:
      return i18n::format<Key::ArgMissingRequiredArgument>(language,
                                                           error.context);
    case arg::ErrorCode::DuplicateOption:
      return i18n::format<Key::ArgDuplicateOption>(language, error.context);
    case arg::ErrorCode::InvalidChoice:
      return i18n::format<Key::ArgInvalidChoice>(language, error.context,
                                                 error.value);
    case arg::ErrorCode::InvalidValue:
      return i18n::format<Key::ArgInvalidValue>(language, error.context,
                                                error.value);
    case arg::ErrorCode::None:
      // The parser reported no reason, so there is none to report. The
      // block is still an error, so a reader is not left wondering
      // whether the run succeeded.
      return i18n::format<Key::CliInvalidConfiguration>(language);
  }
  UNREACHABLE();
}

// The flag spelling a suggestion names, or nothing. A suggestion belongs
// to the error it explains, so it joins that error's message. An index
// past the errors matches nothing, which keeps a stale suggestion from
// rendering against the wrong error.
std::string parse_error_suggestion(
    usize index,
    std::span<const FlagSuggestion> suggestions) {
  for (const FlagSuggestion& suggestion : suggestions) {
    if (suggestion.error_index == index) {
      return fmt::format("--{}", suggestion.flag);
    }
  }
  return "";
}

}  // namespace

std::vector<std::string> render_parse_errors(
    std::string_view command_name,
    const std::vector<arg::ParseError>& errors,
    std::span<const FlagSuggestion> suggestions,
    term::ColorStyle color_style,
    i18n::Language language) {
  const diag::RenderOptions options{
      .color = color_style != term::ColorStyle::Off,
      .language = language,
  };
  std::vector<std::string> blocks;
  blocks.reserve(errors.size() + 1);
  for (usize i = 0; i < errors.size(); ++i) {
    std::string message = parse_error_message(errors[i], language);
    std::string suggestion = parse_error_suggestion(i, suggestions);
    if (!suggestion.empty()) {
      message = i18n::format<i18n::Key::CliDidYouMean>(
          language, std::move(message), std::move(suggestion));
    }
    blocks.push_back(
        diag::render(diag::message(diag::Severity::Error, message), options));
  }
  // Where to read more is advice rather than an error, so it is a note:
  // the same marker, with a word that says which it is.
  blocks.push_back(diag::render(
      diag::message(diag::Severity::Note, i18n::format<i18n::Key::ArgTryHelp>(
                                              language, command_name)),
      options));
  return blocks;
}

}  // namespace cli
