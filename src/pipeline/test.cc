// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pipeline/test.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "diag/bag.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/io/temp_dir.h"
#include "i18n/messages.h"
#include "lowering/lowering.h"
#include "path/path.h"
#include "pipeline/artifact.h"
#include "pipeline/build.h"
#include "pipeline/build_plan.h"
#include "pipeline/diag_code.h"
#include "pipeline/emit_mode.h"
#include "pipeline/pipeline_context.h"
#include "pipeline/spawn.h"
#include "testgen/driver.h"
#include "testgen/tests.h"

namespace pipeline {

namespace {

// A filter names a run by any part of a test's report name, so that
// `--filter vector` reaches every test of the `vector` module and
// `--filter push` reaches the ones whose own name says `push`.
bool keeps(std::string_view filter, std::string_view label) {
  if (filter.empty()) {
    return true;
  }
  return label.find(filter) != std::string_view::npos;
}

// Starts a test process the way the run watches tests, capturing its
// standard output out of the stream the run's own document writes to when
// it asked for that, and returning what the run learned.
base::Result<SpawnOutcome, diag::Reported> spawn_watched(
    PipelineContext& ctx,
    const std::string& exe_path,
    std::span<const std::string> argv_suffix,
    const TestOptions& options,
    TestSummary& summary) {
  std::vector<std::string> argv;
  argv.reserve(argv_suffix.size() + 1);
  argv.emplace_back(exe_path);
  for (const std::string& arg : argv_suffix) {
    argv.emplace_back(arg);
  }
  if (options.capture_output) {
    base::Result<CapturedSpawn, SpawnError> captured =
        run_command_capture(argv);
    if (captured.is_err()) {
      (void)ctx.bag.emit<i18n::Key::PipelineCannotExecute>(
          DiagCode::CannotStartProcess, exe_path);
      return base::make_err(diag::Reported{});
    }
    CapturedSpawn watched = std::move(captured).unwrap();
    summary.output_bytes += watched.output_bytes;
    return base::make_ok(std::move(watched).outcome);
  }
  base::Result<SpawnOutcome, SpawnError> executed = run_command(argv);
  if (executed.is_err()) {
    (void)ctx.bag.emit<i18n::Key::PipelineCannotExecute>(
        DiagCode::CannotStartProcess, exe_path);
    return base::make_err(diag::Reported{});
  }
  return base::make_ok(std::move(executed).unwrap());
}

// Runs one test in a process of its own, which is what lets the run go on
// past one that aborts.
base::Result<void, diag::Reported> execute_test(PipelineContext& ctx,
                                                const std::string& exe_path,
                                                usize index,
                                                std::string_view label,
                                                const TestOptions& options,
                                                TestSummary& summary) {
  const std::string argument = std::to_string(index);
  const std::span<const std::string> argv{&argument, 1};
  base::Result<SpawnOutcome, diag::Reported> spawned =
      spawn_watched(ctx, exe_path, argv, options, summary);
  if (spawned.is_err()) {
    return base::make_err(diag::Reported{});
  }
  const SpawnOutcome ending = std::move(spawned).unwrap();
  TestEvent event{
      .label = label, .ending = TestEnding::Passed, .code = ending.code};
  if (ending.signaled) {
    event.ending = TestEnding::Aborted;
    ++summary.failed;
  } else if (ending.code != 0) {
    event.ending = TestEnding::Failed;
    ++summary.failed;
  } else {
    ++summary.passed;
  }
  if (options.report != nullptr) {
    options.report(event, options.report_ctx);
  }
  return base::make_ok();
}

// Runs every selected test in one process. A failure ends the process, so
// the run learns that it stopped and how, but not which test stopped it:
// the process itself is the only witness, and it is gone.
base::Result<void, diag::Reported> execute_in_one_process(
    PipelineContext& ctx,
    const std::string& exe_path,
    std::span<const usize> chosen,
    const TestOptions& options,
    TestSummary& summary) {
  std::vector<std::string> arguments;
  arguments.reserve(chosen.size());
  for (const usize index : chosen) {
    arguments.emplace_back(std::to_string(index));
  }
  const std::span<const std::string> argv{arguments};
  base::Result<SpawnOutcome, diag::Reported> spawned =
      spawn_watched(ctx, exe_path, argv, options, summary);
  if (spawned.is_err()) {
    return base::make_err(diag::Reported{});
  }
  const SpawnOutcome ending = std::move(spawned).unwrap();
  if (ending.signaled || ending.code != 0) {
    summary.ended_early = true;
    return base::make_ok();
  }
  summary.passed = summary.total;
  return base::make_ok();
}

}  // namespace

std::vector<usize> filtered_tests(std::span<const testgen::TestCase> tests,
                                  std::string_view filter) {
  std::vector<usize> chosen;
  for (usize i = 0; i < tests.size(); ++i) {
    if (keeps(filter, tests[i].label)) {
      chosen.push_back(i);
    }
  }
  return chosen;
}

base::Result<TestSummary, diag::Reported> run_tests(
    PipelineContext& ctx,
    const TestBuild& build,
    TestPackage& package,
    std::span<const usize> chosen,
    const TestOptions& options) {
  TestSummary summary;
  summary.total = static_cast<u32>(chosen.size());
  // A run that stops before executing leaves its executable where a build
  // would have, so that a test program can be inspected; a run that
  // executes puts it in a scratch directory it deletes afterwards,
  // because a run leaves no artifact behind. The scratch directory
  // outlives the execution below, which is what keeps the executable
  // there while it runs.
  std::optional<io::TempDir> scratch;
  std::string exe_path;
  if (options.build_only) {
    const path::Path out_dir = build.root.join("out");
    if (ensure_directories(ctx, out_dir.as_view()).is_err()) {
      return base::make_err(diag::Reported{});
    }
    const path::Path path = out_dir.join(std::string(package.artifact.name) +
                                         "-test" + std::string(exe_suffix()));
    exe_path = std::string(path.as_view());
  } else {
    scratch.emplace(io::TempDir::create_unique("alcy_test_"));
    exe_path = scratch->join(std::string("driver") + std::string(exe_suffix()));
  }
  const PlannedArtifact planned{&package.artifact, EmitMode::Executable,
                                exe_path};
  base::Result<lowering::LoweredPackage, diag::Reported> lowered =
      lower_target(ctx, planned, build.root, build.manifest_file);
  if (lowered.is_err() || ctx.bag.has_errors()) {
    return base::make_err(diag::Reported{});
  }
  lowering::LoweredPackage compiled = std::move(lowered).unwrap();
  if (emit_target(ctx, planned, build.plan, compiled).is_err() ||
      ctx.bag.has_errors()) {
    return base::make_err(diag::Reported{});
  }
  summary.exe_path = exe_path;
  if (options.build_only) {
    // Nothing to start: the build was the whole of the run.
    return base::make_ok(summary);
  }
  // What a run executes leaves no artifact behind: the executable lives in a
  // scratch directory the run deletes at the end, so a path to it is
  // worthless to whoever reads the summary once the run is done.
  summary.exe_path.clear();
  if (chosen.empty()) {
    // Nothing to start: the build was the whole of the run.
    return base::make_ok(summary);
  }
  if (options.execution == testgen::Execution::SingleThread) {
    if (execute_in_one_process(ctx, exe_path, chosen, options, summary)
            .is_err()) {
      return base::make_err(diag::Reported{});
    }
    return base::make_ok(summary);
  }
  for (const usize index : chosen) {
    if (execute_test(ctx, exe_path, index, package.tests[index].label, options,
                     summary)
            .is_err()) {
      return base::make_err(diag::Reported{});
    }
  }
  return base::make_ok(summary);
}

}  // namespace pipeline
