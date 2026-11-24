// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "diag/bag.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "path/path.h"
#include "pipeline/artifact.h"
#include "pipeline/build_plan.h"
#include "pipeline/pipeline_context.h"
#include "source/source.h"
#include "testgen/driver.h"
#include "testgen/tests.h"

namespace pipeline {

// What became of one test, as the run watched it rather than as it read it
// afterwards: a run reports a test when it reaches the end of it, so one
// that stops early still reports the tests it got through.
enum class TestEnding : u8 {
  Passed,
  // The test's process exited with a nonzero status.
  Failed,
  // A signal ended the test's process.
  Aborted,
};

struct TestEvent {
  // The test's name within the package, as `<module path>::<test name>`.
  std::string_view label;
  TestEnding ending = TestEnding::Passed;
  // The exit status, or the signal number for `Aborted`.
  i32 code = 0;
  // How long the test's process ran. Empty when the run could not time
  // it: a single-process run has no witness of an individual test's end,
  // so it reports no events at all rather than events without a measure.
  std::optional<u64> duration_ns;
};

// Reports one test as the run reaches its end. A single-process run reports
// nothing: the process that knows which test ran is the one that panicked,
// and it is gone by then.
using ReportTest = void (*)(const TestEvent& event, const void* ctx);

// The build a run performs before it executes anything: the package's
// directory, the manifest that names its modules, and the plan the command
// resolved for them. Owns its values so that a caller can build one inline
// without borrowing anything that may be a temporary.
struct TestBuild {
  path::Path root;
  source::FileId manifest_file;
  BuildPlan plan;
};

struct TestOptions {
  testgen::Execution execution = testgen::Execution::MultiProcess;
  // Writes the run's executable and stops before executing it.
  bool build_only = false;
  ReportTest report = nullptr;
  const void* report_ctx = nullptr;
  // Keeps each test process's standard output out of the stream this
  // process renders its own output on, and counts the bytes here. Set by a
  // caller whose own output is a document standard output carries: a test
  // that prints would otherwise write through it.
  bool capture_output = false;
};

struct TestSummary {
  // The tests the run selected, whether or not it reached them all.
  u32 total = 0;
  u32 passed = 0;
  u32 failed = 0;
  // Set when one process carried every test and ended before the last of
  // them, which is what a failure inside a test looks like from outside:
  // that process is gone, and the status it exited with is all a run learns
  // from it. The counts stay at the selection and the verdicts the run
  // reached, so a reader cannot take the passes for the whole selection.
  bool ended_early = false;
  // The executable the run built, kept only when the run stopped before
  // executing it and left it where a build would have: a run that executes
  // leaves its executable in a scratch directory the run deletes.
  std::string exe_path;
  // The bytes the selected tests wrote to standard output, counted only
  // when a run asked to capture them; zero otherwise, which reads the way
  // an unmeasured peak does.
  u64 output_bytes = 0;

  bool ok() const { return failed == 0 && !ended_early; }
};

// The positions of the tests the filter keeps, in discovery order. A caller
// that lists a run and one that performs it choose their tests here, so the
// two cannot disagree about what a filter names.
std::vector<usize> filtered_tests(std::span<const testgen::TestCase> tests,
                                  std::string_view filter);

// Builds `package` as `build` says, with the driver on top of it, and
// executes the tests at `chosen`, which are positions in `package.tests`.
// A failure to build reports a diagnostic; a test that fails is a summary,
// because a failing test is what the run was for.
base::Result<TestSummary, diag::Reported> run_tests(
    PipelineContext& ctx,
    const TestBuild& build,
    TestPackage& package,
    std::span<const usize> chosen,
    const TestOptions& options);

}  // namespace pipeline
