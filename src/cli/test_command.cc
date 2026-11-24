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
#include "cli/test_output.h"
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
#include "testgen/tests.h"

namespace cli {

namespace {

// The word a test count takes, singular for one.
std::string_view count_word(usize count, i18n::Language language) {
  return count == 1 ? i18n::text<i18n::Key::CliTestSingular>(language)
                    : i18n::text<i18n::Key::CliTestPlural>(language);
}

// Where the run's progress goes, and what the stream can show.
struct Progress {
  const Logger& err;
  diag::RenderOptions options;
};

// Reports one test as it finishes, from the process that watched it. The
// report is a line on standard error, because standard output belongs to the
// program under test: a test that prints must not have its own output
// interleaved with the lines about it.
void report_test(const pipeline::TestEvent& event, const void* ctx) {
  const auto& progress = *static_cast<const Progress*>(ctx);
  progress.err.block(render_test_case(event, progress.options));
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

  base::Result<pipeline::TargetSet, diag::Reported> targets =
      pipeline::resolve_targets(ctx, raw_dir, false);
  if (targets.is_err()) {
    return failed;
  }
  pipeline::TargetSet chosen_targets = std::move(targets).unwrap();

  // A test run is the only command whose `when(testing)` gate is true.
  // Set it before any file is read so the parser drops items of every
  // other build before resolution sees them.
  ctx.testing = true;
  // The run's front half consults and stores the build's cache, so the
  // opt-out the build carries is the run's too.
  ctx.no_cache = config.no_cache;
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
  const Progress progress{err, announce_options};
  const bool runs = !config.test_no_run;

  pipeline::TestSummary total;
  std::string built_exe;
  for (usize member_index = 0; member_index < chosen_targets.members.size();
       ++member_index) {
    const pipeline::ManifestProbe& member =
        chosen_targets.members[member_index];
    base::Result<pkg::Toolchain, diag::Reported> toolchain =
        pipeline::load_toolchain(ctx, member.root);
    if (toolchain.is_err()) {
      return failed;
    }
    const pkg::Toolchain tool = std::move(toolchain).unwrap();
    // A test is a process the run starts and watches; a target that cannot
    // start one has nothing to report tests with. Freestanding ELF targets
    // can: their entry reads argc and argv off the initial stack.
    ctx.freestanding = config.freestanding || tool.freestanding ||
                       force_freestanding_elf(ctx.target.triple);
    if (ctx.target.is_wasm() ||
        (ctx.freestanding && !force_freestanding_elf(ctx.target.triple))) {
      const u32 index = ctx.bag.emit<i18n::Key::CliTestTargetNotRunnable>(
          pipeline::DiagCode::TestTargetNotRunnable, ctx.target.triple);
      (void)index;
      return failed;
    }
    pkg::Toolchain link_tool = tool;
    link_tool.freestanding = ctx.freestanding;
    const pipeline::LinkOptions link =
        resolve_link_options(config, link_tool, ctx.target.triple);
    base::Result<pkg::Profiles, diag::Reported> profiles =
        pipeline::load_profiles(ctx, member.root);
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
    const pipeline::BuildPlan plan{.profile = selected, .link = link};

    base::Result<pipeline::TestPackage, diag::Reported> package =
        pipeline::resolve_test_package(ctx, member.root, member.manifest,
                                       member.manifest_name,
                                       config.test_execution, observer);
    if (package.is_err()) {
      return failed;
    }
    pipeline::TestPackage built = std::move(package).unwrap();
    // A suite's test names would collide across members, so the member's
    // declared path becomes part of every label. A package keeps its
    // short `<module>::<name>` labels.
    if (chosen_targets.is_suite) {
      const std::string prefix =
          std::string(chosen_targets.suite.packages[member_index]) + "/";
      for (testgen::TestCase& test_case : built.tests) {
        test_case.label.insert(0, prefix);
      }
    }
    const std::vector<usize> chosen =
        pipeline::filtered_tests(built.tests, config.test_filter);
    if (config.test_list) {
      for (const usize index : chosen) {
        envelope.test_labels.emplace_back(built.tests[index].label);
      }
      continue;
    }
    // A member with no selected tests has nothing to announce; in a
    // suite of many packages one empty heading per member would bury the
    // few lines that carry a verdict.
    if (runs && (!chosen_targets.is_suite || !chosen.empty())) {
      const std::string subject = i18n::format<i18n::Key::CliTestRunning>(
          language, chosen.size(), count_word(chosen.size(), language),
          built.artifact.name);
      err.block(render_test_heading(subject, progress.options));
    }
    const pipeline::TestOptions options{
        .execution = config.test_execution,
        .build_only = config.test_no_run,
        .report = runs ? report_test : nullptr,
        .report_ctx = runs ? static_cast<const void*>(&progress) : nullptr,
        .capture_output = config.json,
    };
    base::Result<pipeline::TestSummary, diag::Reported> summary =
        pipeline::run_tests(ctx, {member.root, member.manifest, plan}, built,
                            chosen, options);
    if (summary.is_err()) {
      return failed;
    }
    const pipeline::TestSummary done = std::move(summary).unwrap();
    total.total += done.total;
    total.passed += done.passed;
    total.failed += done.failed;
    total.output_bytes += done.output_bytes;
    total.ended_early = total.ended_early || done.ended_early;
    if (!runs) {
      built_exe = done.exe_path;
    }
  }
  envelope.trace = trace.take_events();
  envelope.cache = ctx.cache_outcome;

  if (config.test_list) {
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
    envelope.link_sdk_path = ctx.link_sdk_path;
    envelope.status = Status::Ok;
    envelope.outcome = Outcome::Listed;
    return result_code(ResultCode::Success);
  }

  envelope.test_ran = true;
  envelope.test_count = total.total;
  envelope.test_failed = total.failed;
  envelope.test_ended_early = total.ended_early;
  envelope.output_bytes = total.output_bytes;
  if (!runs) {
    envelope.link_sdk_path = ctx.link_sdk_path;
    envelope.status = Status::Ok;
    envelope.outcome = Outcome::Built;
    envelope.output_path = built_exe;
    return result_code(ResultCode::Success);
  }
  // A failed run still left counts on the envelope. The one summary
  // line is rendered at report time from those counts, so a reader who
  // tails the log sees the verdict whether the run passed or failed.
  if (!total.ok()) {
    return result_code(ResultCode::TestFailed);
  }
  envelope.link_sdk_path = ctx.link_sdk_path;
  envelope.status = Status::Ok;
  envelope.outcome = Outcome::Tested;
  return result_code(ResultCode::Success);
}

}  // namespace cli
