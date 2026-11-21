// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/test_command.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cli/build_progress.h"
#include "cli/cli_config.h"
#include "cli/logger.h"
#include "cli/output.h"
#include "cli/result_code.h"
#include "cli/trace.h"
#include "codegen/target.h"
#include "diag/bag.h"
#include "diag/render.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/io/io_util.h"
#include "fpag/term/color_style.h"
#include "fpag/term/console.h"
#include "i18n/language.h"
#include "i18n/messages.h"
#include "pipeline/artifact.h"
#include "pipeline/build_plan.h"
#include "pipeline/diag_code.h"
#include "pipeline/link_options.h"
#include "pipeline/pipeline_context.h"
#include "pipeline/profile.h"
#include "pipeline/test.h"
#include "pkg/manifest.h"
#include "pkg/profile.h"
#include "pkg/toolchain.h"

namespace cli {

namespace {

// The word a test count takes, singular for one.
std::string_view count_word(usize count, i18n::Language language) {
  return count == 1 ? i18n::text<i18n::Key::CliTestSingular>(language)
                    : i18n::text<i18n::Key::CliTestPlural>(language);
}

// Where the run's progress goes, and in which language.
struct Progress {
  const Logger& err;
  i18n::Language language;
};

// Reports one test as it finishes, from the process that watched it. The
// report is a line on standard error, because standard output belongs to the
// program under test: a test that prints must not have its own output
// interleaved with the lines about it.
void report_test(const pipeline::TestEvent& event, const void* ctx) {
  const auto& progress = *static_cast<const Progress*>(ctx);
  std::string line;
  switch (event.ending) {
    case pipeline::TestEnding::Passed:
      line = i18n::format<i18n::Key::CliTestCaseOk>(progress.language,
                                                    event.label);
      break;
    case pipeline::TestEnding::Failed:
      line = i18n::format<i18n::Key::CliTestCaseFailed>(
          progress.language, event.label, event.code);
      break;
    case pipeline::TestEnding::Aborted:
      line = i18n::format<i18n::Key::CliTestCaseAborted>(
          progress.language, event.label, event.code);
      break;
  }
  line += '\n';
  progress.err.block(line);
}

}  // namespace

i32 run_test(const CliConfig& config,
             pipeline::PipelineContext& ctx,
             Envelope& envelope) {
  TraceSession trace(ctx, config.time_trace);
  envelope.bag = &ctx.bag;
  envelope.sources = &ctx.sources;
  // A package that does not build is a check failure: the tests never had a
  // chance to answer.
  const i32 failed = result_code(ResultCode::CheckFailed);
  // Validation keeps single files out: `test` takes a package directory,
  // because a test is discovered from the modules a manifest selects.
  const std::string_view raw_dir = config.input.empty() ? "." : config.input;
  const i18n::Language language = config.language;

  base::Result<pipeline::ManifestProbe, diag::Reported> probe =
      pipeline::require_package_manifest(ctx, raw_dir, false);
  if (probe.is_err()) {
    return failed;
  }
  pipeline::ManifestProbe found = std::move(probe).unwrap();
  // A suite names the packages that carry the tests rather than carrying
  // them itself, so it is not something one run can test.
  const std::optional<std::string_view> manifest_bytes =
      ctx.sources.bytes(found.manifest);
  if (manifest_bytes.has_value() &&
      pkg::probe_manifest_kind(*manifest_bytes) == pkg::ManifestKind::Suite) {
    const u32 index = ctx.bag.emit<i18n::Key::CliTestSuiteNotSupported>(
        pipeline::DiagCode::TestSuiteGiven);
    (void)index;
    return failed;
  }
  base::Result<pkg::Toolchain, diag::Reported> toolchain =
      pipeline::load_toolchain(ctx, found.root);
  if (toolchain.is_err()) {
    return failed;
  }
  const pkg::Toolchain tool = std::move(toolchain).unwrap();
  // A test is a process the run starts and watches; a target that cannot
  // start one has nothing to report tests with. Freestanding ELF targets
  // can: their entry reads argc and argv off the initial stack.
  // On those triples the test binary is always the kernel-only link, so
  // the no-libc path keeps its coverage even though a hosted build now
  // links the system C runtime by default.
  // A test run is the only command whose `when(testing)` gate is true.
  // Set it before any file is read so the parser drops items of every
  // other build before resolution sees them.
  ctx.testing = true;
  ctx.freestanding =
      tool.freestanding || force_freestanding_elf(ctx.target.triple);
  if (ctx.target.is_wasm() ||
      (ctx.freestanding && !force_freestanding_elf(ctx.target.triple))) {
    const u32 index = ctx.bag.emit<i18n::Key::CliTestTargetNotRunnable>(
        pipeline::DiagCode::TestTargetNotRunnable, ctx.target.triple);
    (void)index;
    return failed;
  }
  // The run's front half consults and stores the build's cache, so the
  // opt-out the build carries is the run's too.
  ctx.no_cache = config.no_cache;
  // Emission and the link must agree on the link kind, so the file the
  // link sees carries the choice this run made.
  pkg::Toolchain link_tool = tool;
  link_tool.freestanding = ctx.freestanding;
  const pipeline::LinkOptions link =
      resolve_link_options(config, link_tool, ctx.target.triple);
  base::Result<pkg::Profiles, diag::Reported> profiles =
      pipeline::load_profiles(ctx, found.root);
  if (profiles.is_err()) {
    return failed;
  }
  const pkg::Profiles tables = std::move(profiles).unwrap();
  base::Result<pipeline::BuildProfile, diag::Reported> profile =
      pipeline::resolve_profile(
          pipeline::ProfileRequest{.name = config.profile,
                                   .on_panic = config.on_panic,
                                   .optimize = config.optimize,
                                   .overflow_checks = config.overflow_checks},
          tables, ctx.bag);
  if (profile.is_err()) {
    return failed;
  }
  const pipeline::BuildProfile selected = std::move(profile).unwrap();
  resolve_backend_options(config, ctx, selected);
  FdSink err_sink{io::STDERR_FD};
  const Logger err{&FdSink::write, &err_sink};
  const term::ColorStyle style =
      term::console_color_style(term::Stream::Stderr, config.color_mode);
  const diag::RenderOptions announce_options{
      .color = style != term::ColorStyle::Off,
      .language = language,
  };
  const BuildProgress build_progress{err, !config.json && !config.test_list,
                                     announce_options};
  const pipeline::BuildProgressObserver build_observer =
      build_progress_observer(build_progress);
  const pipeline::BuildProgressObserver* observer =
      config.test_list ? nullptr : &build_observer;
  const pipeline::BuildPlan plan{.profile = selected, .link = link};

  base::Result<pipeline::TestPackage, diag::Reported> package =
      pipeline::resolve_test_package(ctx, found.root, found.manifest,
                                     found.manifest_name, config.test_execution,
                                     observer);
  if (package.is_err()) {
    return failed;
  }
  pipeline::TestPackage built = std::move(package).unwrap();
  // The parent owns the selection: it lists the run, it names the tests the
  // run performs, and the driver inside selects by position.
  const std::vector<usize> chosen =
      pipeline::filtered_tests(built.tests, config.test_filter);
  envelope.test_count = chosen.size();
  if (config.test_list) {
    envelope.test_labels.reserve(chosen.size());
    for (const usize index : chosen) {
      envelope.test_labels.emplace_back(built.tests[index].label);
    }
    // The document carries a listing's labels itself, and the text report
    // is the lines a person reads. A run that printed both would put a
    // second thing on standard output beside the document.
    if (!config.json) {
      FdSink out_sink{io::STDOUT_FD};
      const Logger out{&FdSink::write, &out_sink};
      for (const std::string& label : envelope.test_labels) {
        out.block(label + '\n');
      }
    }
    envelope.status = Status::Ok;
    envelope.outcome = Outcome::Listed;
    return result_code(ResultCode::Success);
  }
  const Progress progress{err, language};
  const bool runs = !config.test_no_run;
  if (runs) {
    err.block(i18n::format<i18n::Key::CliTestRunning>(
                  language, chosen.size(), count_word(chosen.size(), language),
                  built.artifact.name) +
              '\n');
  }
  // A document is on standard output, so a test process's own bytes have
  // to stay off it; a run of prose sends the tests' bytes to the terminal.
  const pipeline::TestOptions options{
      .execution = config.test_execution,
      .build_only = config.test_no_run,
      .report = runs ? report_test : nullptr,
      .report_ctx = runs ? static_cast<const void*>(&progress) : nullptr,
      .capture_output = config.json,
  };
  base::Result<pipeline::TestSummary, diag::Reported> summary =
      pipeline::run_tests(ctx, {found.root, found.manifest, plan}, built,
                          chosen, options);
  envelope.trace = trace.take_events();
  envelope.cache = ctx.cache_outcome;
  if (summary.is_err()) {
    return failed;
  }
  const pipeline::TestSummary done = std::move(summary).unwrap();
  envelope.test_count = done.total;
  envelope.test_failed = done.failed;
  envelope.test_ended_early = done.ended_early;
  envelope.output_bytes = done.output_bytes;
  if (!runs) {
    envelope.status = Status::Ok;
    envelope.outcome = Outcome::Built;
    envelope.output_path = done.exe_path;
    return result_code(ResultCode::Success);
  }
  if (done.ended_early) {
    // The run died inside a test, so the ones after it have no verdict;
    // a count of passes would read as if they all passed.
    err.block(i18n::format<i18n::Key::CliTestStopped>(language) + '\n');
    return result_code(ResultCode::TestFailed);
  }
  err.block(i18n::format<i18n::Key::CliTestPassed>(
                language, done.passed, count_word(done.passed, language)) +
            '\n');
  if (done.failed > 0) {
    err.block(i18n::format<i18n::Key::CliTestFailed>(
                  language, done.failed, count_word(done.failed, language)) +
              '\n');
  }
  if (!done.ok()) {
    return result_code(ResultCode::TestFailed);
  }
  envelope.status = Status::Ok;
  envelope.outcome = Outcome::Tested;
  return result_code(ResultCode::Success);
}

}  // namespace cli
