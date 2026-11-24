// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string>
#include <string_view>

#include "i18n/language.h"
#include "pipeline/test.h"

namespace cli {

// Which part of a test run a line reports, and so which emphasis it
// carries. The text is the message catalog's wording; this decides only
// how it is printed.
enum class TestLineStyle {
  Heading,
  Passed,
  Failed,
  Stopped,
};

// One finished line, newline included, for `text` in `style`. With
// `color` false the line is the plain catalog text, which is what a
// redirected stream and a non-terminal reader see.
std::string render_test_line(std::string_view text,
                             TestLineStyle style,
                             bool color);

// One finished line, newline included, for a test that ended. The
// duration rides after the verdict: `format_duration`'s wording, or
// "unmeasured" when the run had no witness of the test's process.
std::string render_test_case(const pipeline::TestEvent& event,
                             i18n::Language language,
                             bool color);

}  // namespace cli
