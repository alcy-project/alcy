// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/parse_output.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "cli/cli_config.h"
#include "cli/logger.h"
#include "cli/parse_args.h"
#include "cli/result_code.h"
#include "cli/usage.h"
#include "doctest/doctest.h"
#include "fpag/arg/parser.h"
#include "fpag/base/numeric.h"
#include "fpag/term/color_mode.h"
#include "fpag/term/color_style.h"
#include "i18n/language.h"

namespace cli {

namespace {

ParseOutcome parse(std::span<const std::string_view> args) {
  arg::Parser parser = build_parser();
  return parse_args(parser, args);
}

Interruption render(ParseOutcome&& outcome) {
  arg::Parser parser = build_parser();
  return render_outcome(parser, outcome, term::ColorMode::Never);
}

// Whether every style a line opens is closed before the line ends. A
// style that survives its line paints whatever comes after it, which is
// how a reset the formatter forgot turns up in the output.
bool styles_balanced(std::string_view text) {
  isize open = 0;
  usize i = 0;
  while (i < text.size()) {
    if (text[i] == '\n') {
      if (open != 0) {
        return false;
      }
      ++i;
      continue;
    }
    if (text[i] == '\x1b' && i + 1 < text.size() && text[i + 1] == '[') {
      const usize end = text.find('m', i + 2);
      if (end == std::string_view::npos) {
        return false;
      }
      const std::string_view parameters = text.substr(i + 2, end - i - 2);
      // One `0` closes every attribute, so it clears the count rather than
      // closing one of them.
      open = parameters == "0" ? 0 : open + 1;
      i = end + 1;
      continue;
    }
    ++i;
  }
  return open == 0;
}

// The answer and the errors as one string, for the cases that assert on a
// phrase. Which stream a phrase landed on is a separate question, and
// `render` is what the split is tested with.
std::string flatten(const Interruption& interruption) {
  std::string text = interruption.text;
  for (const std::string& error : interruption.errors) {
    text += error;
  }
  return text;
}

}  // namespace

TEST_CASE("Render help for explicit and bare invocations") {
  const std::string_view help[] = {"alcy", "--help"};
  const Interruption asked = render(parse(help));
  CHECK(asked.text.find("build") != std::string::npos);
  CHECK(asked.errors.empty());

  const std::string_view bare[] = {"alcy"};
  const Interruption none = render(parse(bare));
  CHECK(none.text.find("build") != std::string::npos);
  CHECK(none.errors.empty());
}

// Every style a coloured line opens is closed before the line ends. A
// `Commands:` that forgot its reset underlined the list under it, which
// is what this pins down.
TEST_CASE("Coloured help closes every style it opens") {
  const HelpFormatter help{i18n::Language::EnUs};
  const std::string text =
      help(build_parser().root_command(), term::ColorStyle::Ansi16);
  CHECK(text.find("\x1b[") != std::string::npos);
  CHECK(styles_balanced(text));
}

TEST_CASE("Render version") {
  const std::string_view args[] = {"alcy", "--version"};
  arg::Parser parser = build_parser();
  const Interruption interruption =
      render_outcome(parser, parse(args), term::ColorMode::Never);
  CHECK(interruption.text.find(std::string(parser.root_command().version())) !=
        std::string::npos);
  CHECK(interruption.errors.empty());
}

// An answer is standard output and an error is standard error, so the two
// never arrive in one interruption. What decides it is whether the
// invocation asked a question or made a mistake.
TEST_CASE("An answer and an error are never the same interruption") {
  const std::string_view question[] = {"alcy", "--help"};
  CHECK(render(parse(question)).errors.empty());

  const std::string_view mistake[] = {"alcy", "frobnicate"};
  const Interruption unknown = render(parse(mistake));
  CHECK(unknown.text.empty());
  CHECK(unknown.errors.size() == 1);
  CHECK(unknown.errors[0].find("frobnicate") != std::string::npos);
  CHECK(unknown.errors[0].find("--help") != std::string::npos);

  const std::string_view bad_flag[] = {"alcy", "build", "--frobnicator"};
  const Interruption failed = render(parse(bad_flag));
  CHECK(failed.text.empty());
  // The rejected flag, then where to read more, which is a note rather
  // than a second error.
  CHECK(failed.errors.size() == 2);
  CHECK(failed.errors[0].rfind("error: ", 0) == 0);
  CHECK(failed.errors[1].rfind("note: ", 0) == 0);
}

TEST_CASE("Render subcommand suggestion") {
  const std::string_view close[] = {"alcy", "buid"};
  CHECK(flatten(render(parse(close))).find("did you mean 'build'?") !=
        std::string::npos);

  const std::string_view far[] = {"alcy", "frobnicate"};
  CHECK(flatten(render(parse(far))).find("did you mean") == std::string::npos);
}

TEST_CASE("Render flag suggestion") {
  const std::string_view close[] = {"alcy", "build", "--outpu"};
  CHECK(flatten(render(parse(close))).find("did you mean '--output'?") !=
        std::string::npos);

  const std::string_view far[] = {"alcy", "build", "--frobnicator"};
  CHECK(flatten(render(parse(far))).find("did you mean") == std::string::npos);
}

// Every block an interruption hands to a writer is a finished block. The
// version line and the unknown-subcommand line are the two a formatter
// hands over as bare sentences, so they are the two a newline has to be
// added to.
TEST_CASE("Every block an interruption carries is finished") {
  const std::string_view help[] = {"alcy", "--help"};
  CHECK(is_block(render(parse(help)).text));

  const std::string_view bare[] = {"alcy"};
  CHECK(is_block(render(parse(bare)).text));

  const std::string_view version[] = {"alcy", "--version"};
  CHECK(is_block(render(parse(version)).text));

  const std::string_view unknown[] = {"alcy", "frobnicate"};
  for (const std::string& error : render(parse(unknown)).errors) {
    CHECK(is_block(error));
  }

  const std::string_view suggested[] = {"alcy", "buid"};
  for (const std::string& error : render(parse(suggested)).errors) {
    CHECK(is_block(error));
  }

  const std::string_view bad_flag[] = {"alcy", "build", "--nonsense"};
  for (const std::string& error : render(parse(bad_flag)).errors) {
    CHECK(is_block(error));
  }
}

TEST_CASE("A successful parse interrupts nothing") {
  const std::string_view args[] = {"alcy", "build"};
  const Interruption interruption = render(parse(args));
  CHECK(interruption.text.empty());
  CHECK(interruption.errors.empty());
}

TEST_CASE("Interruption exit codes") {
  const std::string_view build[] = {"alcy", "build"};
  const std::string_view bare[] = {"alcy"};
  const std::string_view help[] = {"alcy", "--help"};
  const std::string_view version[] = {"alcy", "--version"};
  const std::string_view unknown[] = {"alcy", "frobnicate"};
  const std::string_view broken[] = {"alcy", "build", "--frobnicator"};

  CHECK(parse(build).is<CliConfig>());
  CHECK(interruption_exit_code(parse(build)) == std::nullopt);
  CHECK(interruption_exit_code(parse(bare)) == ResultCode::Success);
  CHECK(interruption_exit_code(parse(help)) == ResultCode::Success);
  CHECK(interruption_exit_code(parse(version)) == ResultCode::Success);
  CHECK(interruption_exit_code(parse(unknown)) == ResultCode::ArgParseError);
  CHECK(interruption_exit_code(parse(broken)) == ResultCode::ArgParseError);
}

TEST_CASE("Scan color mode") {
  const char* none[] = {"alcy", "build"};
  const char* equals[] = {"alcy", "--color=never", "build"};
  const char* separate[] = {"alcy", "build", "--color", "always"};
  const char* bogus[] = {"alcy", "--color=bogus", "build"};
  CHECK(scan_color_mode(2, none) == term::ColorMode::Auto);
  CHECK(scan_color_mode(3, equals) == term::ColorMode::Never);
  CHECK(scan_color_mode(4, separate) == term::ColorMode::Always);
  CHECK(scan_color_mode(3, bogus) == term::ColorMode::Auto);
  CHECK(scan_color_mode(0, nullptr) == term::ColorMode::Auto);
}

TEST_CASE("Scan language") {
  const char* none[] = {"alcy", "build"};
  const char* equals[] = {"alcy", "--lang=en-us", "build"};
  const char* separate[] = {"alcy", "build", "--lang", "en-us"};
  // A tag the catalogs do not cover is the parser's error to report, not
  // a reason for this scan to report another language.
  const char* unsupported[] = {"alcy", "--lang=ja-jp", "build"};
  const char* miscased[] = {"alcy", "--lang=EN-US", "build"};
  CHECK(scan_language(2, none) == i18n::Language::EnUs);
  CHECK(scan_language(3, equals) == i18n::Language::EnUs);
  CHECK(scan_language(4, separate) == i18n::Language::EnUs);
  CHECK(scan_language(3, unsupported) == i18n::Language::EnUs);
  CHECK(scan_language(3, miscased) == i18n::Language::EnUs);
  CHECK(scan_language(0, nullptr) == i18n::Language::EnUs);
}

TEST_CASE("The language reaches the config") {
  const std::string_view args[] = {"alcy", "--lang=en-us", "build"};
  CHECK(parse(args).get<CliConfig>().language == i18n::Language::EnUs);
}

TEST_CASE("A tag with no catalog is rejected") {
  const std::string_view args[] = {"alcy", "--lang=ja-jp", "build"};
  CHECK(parse(args).is<ParseFailure>());
}

}  // namespace cli
