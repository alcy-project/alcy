// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/test_output.h"

#include <string>
#include <string_view>

#include "cli/duration.h"
#include "fpag/term/style.h"
#include "i18n/messages.h"

namespace cli {

namespace {

// A styled span: the suffix is written after the text so a neighbouring
// span starts from a clean state, and nothing here assumes the reader
// can tell two colours apart.
void styled(std::string& out,
            std::string_view text,
            std::string_view prefix,
            bool color) {
  if (!color) {
    out.append(text);
    return;
  }
  out.append(prefix);
  out.append(text);
  out.append(term::RESET);
}

}  // namespace

std::string render_test_line(std::string_view text,
                             TestLineStyle style,
                             bool color) {
  std::string prefix;
  switch (style) {
    case TestLineStyle::Heading:
      prefix = term::BOLD;
      prefix += term::FG_CYAN;
      break;
    case TestLineStyle::Passed:
      prefix = term::BOLD;
      prefix += term::FG_BRIGHT_GREEN;
      break;
    case TestLineStyle::Failed:
    case TestLineStyle::Stopped:
      prefix = term::BOLD;
      prefix += term::FG_BRIGHT_RED;
      break;
  }
  std::string out;
  styled(out, text, prefix, color);
  out.push_back('\n');
  return out;
}

std::string render_test_case(const pipeline::TestEvent& event,
                             i18n::Language language,
                             bool color) {
  std::string body;
  std::string_view verdict_color = term::FG_RED;
  switch (event.ending) {
    case pipeline::TestEnding::Passed:
      body = i18n::format<i18n::Key::CliTestCaseOk>(language, event.label);
      verdict_color = term::FG_GREEN;
      break;
    case pipeline::TestEnding::Failed:
      body = i18n::format<i18n::Key::CliTestCaseFailed>(language, event.label,
                                                        event.code);
      break;
    case pipeline::TestEnding::Aborted:
      body = i18n::format<i18n::Key::CliTestCaseAborted>(language, event.label,
                                                         event.code);
      break;
  }
  std::string line;
  styled(line, body, verdict_color, color);
  // The measure follows the verdict, outside its colour: a wrap in a
  // muted tone keeps the verdict the first thing a reader sees, and a
  // terminal without colour still reads the same words in the same
  // order.
  std::string timing = " (";
  if (event.duration_ns.has_value()) {
    timing += format_duration(*event.duration_ns);
  } else {
    timing += i18n::text<i18n::Key::CliTestUnmeasured>(language);
  }
  timing.push_back(')');
  styled(line, timing, term::DIM, color);
  line.push_back('\n');
  return line;
}

}  // namespace cli
