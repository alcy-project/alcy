// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/test_output.h"

#include <string>

#include "doctest/doctest.h"
#include "i18n/language.h"
#include "pipeline/test.h"

namespace cli {

namespace {

constexpr i18n::Language kEn = i18n::Language::EnUs;

pipeline::TestEvent event(pipeline::TestEnding ending,
                          i32 code,
                          u64 duration_ns) {
  pipeline::TestEvent out;
  out.label = "core::adds";
  out.ending = ending;
  out.code = code;
  out.duration_ns = duration_ns;
  return out;
}

}  // namespace

TEST_CASE("A plain test line reads the same with and without a terminal") {
  CHECK(render_test_case(event(pipeline::TestEnding::Passed, 0, 57'900'000),
                         kEn, false) == "test core::adds ... ok (57.9 ms)\n");
  CHECK(render_test_case(event(pipeline::TestEnding::Failed, 2, 1'020'000'000),
                         kEn, false) ==
        "test core::adds ... FAILED (exit status 2) (1.02 s)\n");
  CHECK(render_test_case(event(pipeline::TestEnding::Aborted, 11, 999), kEn,
                         false) ==
        "test core::adds ... FAILED (aborted by signal 11) (999 ns)\n");
}

TEST_CASE("A test the run could not time says so") {
  pipeline::TestEvent unsized = event(pipeline::TestEnding::Passed, 0, 0);
  unsized.duration_ns.reset();
  CHECK(render_test_case(unsized, kEn, false) ==
        "test core::adds ... ok (unmeasured)\n");
}

TEST_CASE("Colour marks the verdict and mutes the duration") {
  const std::string out = render_test_case(
      event(pipeline::TestEnding::Passed, 0, 57'900'000), kEn, true);
  CHECK(out.find("\033[32mtest core::adds ... ok\033[0m") == 0);
  CHECK(out.find("\033[2m (57.9 ms)\033[0m\n") != std::string::npos);

  const std::string failed =
      render_test_line("2 tests failed", TestLineStyle::Failed, true);
  CHECK(failed == "\033[1m\033[91m2 tests failed\033[0m\n");
}

TEST_CASE("A styled line is the plain one when colour is off") {
  CHECK(render_test_line("running 2 tests", TestLineStyle::Heading, false) ==
        "running 2 tests\n");
  CHECK(render_test_line("1 test passed", TestLineStyle::Passed, false) ==
        "1 test passed\n");
  CHECK(render_test_line("the run stopped before every test finished",
                         TestLineStyle::Stopped, false) ==
        "the run stopped before every test finished\n");
}

}  // namespace cli
