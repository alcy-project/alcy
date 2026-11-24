// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/test_output.h"

#include <string>

#include "diag/render.h"
#include "doctest/doctest.h"
#include "i18n/language.h"
#include "pipeline/test.h"

namespace cli {

namespace {

diag::RenderOptions options(bool color) {
  return diag::RenderOptions{.color = color, .language = i18n::Language::EnUs};
}

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
                         options(false)) == "  PASS    core::adds (57.9 ms)\n");
  CHECK(render_test_case(event(pipeline::TestEnding::Failed, 2, 1'020'000'000),
                         options(false)) ==
        "  FAILED  core::adds (exit status 2) (1.02 s)\n");
  CHECK(render_test_case(event(pipeline::TestEnding::Aborted, 11, 999),
                         options(false)) ==
        "  FAILED  core::adds (aborted by signal 11) (999 ns)\n");
}

TEST_CASE("A test the run could not time says so") {
  pipeline::TestEvent unsized = event(pipeline::TestEnding::Passed, 0, 0);
  unsized.duration_ns.reset();
  CHECK(render_test_case(unsized, options(false)) ==
        "  PASS    core::adds (unmeasured)\n");
}

TEST_CASE("Colour marks the verdict and mutes the duration") {
  const std::string out = render_test_case(
      event(pipeline::TestEnding::Passed, 0, 57'900'000), options(true));
  CHECK(out.find("  \033[32mPASS\033[0m    core::adds") == 0);
  CHECK(out.find("\033[2m (57.9 ms)\033[0m\n") != std::string::npos);
}

TEST_CASE("A run opens with the same verb column as `run`") {
  CHECK(render_test_heading("2 tests (package core)", options(false)) ==
        "Running   2 tests (package core)\n");
  CHECK(render_test_heading("2 tests (package core)", options(true)) ==
        "\033[1m\033[92mRunning\033[0m   "
        "\033[4m2 tests (package core)\033[0m\n");
}

}  // namespace cli
