// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/test_output.h"

#include <algorithm>
#include <string>
#include <string_view>

#include "cli/duration.h"
#include "cli/output.h"
#include "fpag/term/style.h"
#include "i18n/language.h"
#include "i18n/messages.h"
#include "pipeline/test.h"

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

std::string render_test_heading(std::string_view subject,
                                const diag::RenderOptions& options) {
  std::string line;
  render_labelled(line, i18n::text<i18n::Key::CliVerbRunning>(options.language),
                  subject, true, options.color, true);
  return line;
}

std::string render_test_case(const pipeline::TestEvent& event,
                             const diag::RenderOptions& options) {
  using i18n::Key;
  const i18n::Language language = options.language;
  const std::string_view passed =
      i18n::text<Key::CliTestVerdictPassed>(language);
  const std::string_view failed =
      i18n::text<Key::CliTestVerdictFailed>(language);
  const bool ok = event.ending == pipeline::TestEnding::Passed;
  const std::string_view verdict = ok ? passed : failed;
  const std::string_view verdict_color = ok ? term::FG_GREEN : term::FG_RED;

  // The verdict is a column, not a word inside a sentence: every path
  // starts at the same character, so a reader scans a run by verdict
  // first and by name second. The width comes from the translated words
  // rather than the English ones, so the column survives a translation.
  const usize width = std::max(passed.size(), failed.size()) + 2;
  std::string line = "  ";
  styled(line, verdict, verdict_color, options.color);
  line.append(width - verdict.size(), ' ');
  line.append(event.label);

  if (event.ending == pipeline::TestEnding::Failed) {
    line.push_back(' ');
    line += i18n::format<Key::CliTestCaseExitStatus>(language, event.code);
  } else if (event.ending == pipeline::TestEnding::Aborted) {
    line.push_back(' ');
    line += i18n::format<Key::CliTestCaseSignal>(language, event.code);
  }

  // The measure follows the name, outside the verdict's colour: the
  // path stays readable on a terminal without colour, and the muted
  // tone keeps the verdict the first thing a reader sees.
  std::string timing = " (";
  if (event.duration_ns.has_value()) {
    timing += format_duration(*event.duration_ns);
  } else {
    timing += i18n::text<Key::CliTestUnmeasured>(language);
  }
  timing.push_back(')');
  styled(line, timing, term::DIM, options.color);
  line.push_back('\n');
  return line;
}

}  // namespace cli
