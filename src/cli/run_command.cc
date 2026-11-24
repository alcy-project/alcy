// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/run_command.h"

#include <optional>
#include <span>
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
#include "diag/bag.h"
#include "diag/render.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/io/io_util.h"
#include "fpag/term/color_style.h"
#include "fpag/term/console.h"
#include "i18n/messages.h"
#include "pipeline/artifact.h"
#include "pipeline/build_plan.h"
#include "pipeline/diag_code.h"
#include "pipeline/link_options.h"
#include "pipeline/pipeline_context.h"
#include "pipeline/profile.h"
#include "pipeline/run.h"
#include "pkg/manifest.h"
#include "pkg/profile.h"
#include "pkg/toolchain.h"

namespace cli {

namespace {

// The announcement is written to standard error, so it is styled by
// what standard error can do rather than by what standard output can:
// redirecting the program's output to a pipe must not strip the label
// of the program that produced it, or vice versa.
struct Announcement {
  const diag::RenderOptions& options;
  const Logger& err;
  const pipeline::PipelineContext& ctx;
  std::string_view prefix = "";
};

void announce_exec(std::string_view program, const void* ctx) {
  const auto& a = *static_cast<const Announcement*>(ctx);
  std::string labeled(a.prefix);
  labeled.append(program);
  announce(a.err, i18n::text<i18n::Key::CliVerbRunning>(a.options.language),
           labeled, a.options);
}

// The build's result goes to standard error before the program starts:
// standard output belongs to the program, and a result line after its
// output would read as more of it.
void announce_built(std::string_view program,
                    u64 duration_ns,
                    const void* ctx) {
  const auto& a = *static_cast<const Announcement*>(ctx);
  std::string subject(a.prefix);
  subject.append(program);
  Envelope built;
  built.status = Status::Ok;
  built.outcome = Outcome::Built;
  built.output_path = std::move(subject);
  built.wall_ns = duration_ns;
  built.cache = a.ctx.cache_outcome;
  a.err.block(render_result_line(built, a.options));
}

}  // namespace

i32 run_run(const CliConfig& config,
            pipeline::PipelineContext& ctx,
            Envelope& envelope) {
  TraceSession trace(ctx, config.time_trace);
  envelope.bag = &ctx.bag;
  envelope.sources = &ctx.sources;
  const i32 failed = result_code(ResultCode::RunFailed);
  const std::string_view raw_dir = config.input.empty() ? "." : config.input;
  const std::span<const std::string_view> args(config.program_args);

  base::Result<pipeline::TargetSet, diag::Reported> targets =
      pipeline::resolve_targets(ctx, raw_dir, false);
  if (targets.is_err()) {
    return failed;
  }
  pipeline::TargetSet chosen = std::move(targets).unwrap();
  std::vector<pipeline::ManifestProbe> run_members;
  std::vector<std::string> prefixes;
  if (chosen.is_suite) {
    if (!chosen.suite.has_default_run || chosen.suite.default_run_count == 0) {
      const std::string suite = pipeline::suite_spelling(chosen.suite);
      const u32 index = ctx.bag.emit<i18n::Key::PipelineSuiteNoDefaultRun>(
          pipeline::DiagCode::NoTargets, suite);
      (void)index;
      return failed;
    }
    for (u32 i = 0; i < chosen.suite.default_run_count; ++i) {
      const std::string_view member = chosen.suite.default_run[i];
      for (u32 j = 0; j < chosen.suite.package_count; ++j) {
        if (chosen.suite.packages[j] == member) {
          run_members.push_back(chosen.members[j]);
          prefixes.push_back(std::string(member) + "/");
          break;
        }
      }
    }
  } else {
    run_members = std::move(chosen.members);
    prefixes.emplace_back();
  }

  ctx.no_cache = config.no_cache;
  const term::ColorStyle style =
      term::console_color_style(term::Stream::Stderr, config.color_mode);
  const diag::RenderOptions announce_options{
      .color = style != term::ColorStyle::Off,
      .language = config.language,
  };
  FdSink err_sink{io::STDERR_FD};
  const Logger err{&FdSink::write, &err_sink};
  const BuildProgress progress{err, true, announce_options};
  const pipeline::BuildProgressObserver progress_observer =
      build_progress_observer(progress);

  i32 first_failure = 0;
  u32 failed_programs = 0;
  for (usize i = 0; i < run_members.size(); ++i) {
    const pipeline::ManifestProbe& found = run_members[i];
    base::Result<pkg::Toolchain, diag::Reported> toolchain =
        pipeline::load_toolchain(ctx, found.root);
    if (toolchain.is_err()) {
      return failed;
    }
    const pkg::Toolchain tool = std::move(toolchain).unwrap();
    // Reset per member: a toolchain choice from one suite member must
    // not change how the next one links.
    ctx.freestanding = config.freestanding || tool.freestanding;
    const pipeline::LinkOptions link =
        resolve_link_options(config, tool, ctx.target.triple);
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
    const Announcement announcement{announce_options, err, ctx, prefixes[i]};
    const pipeline::BuildPlan plan{.profile = selected, .link = link};
    base::Result<pipeline::RunOutcome, diag::Reported> result =
        pipeline::run_package(
            ctx, found.root, found.manifest, found.manifest_name, plan, args,
            announce_exec, &announcement, &progress_observer, announce_built);
    if (result.is_err()) {
      return failed;
    }
    const i32 code = std::move(result).unwrap().exit_code;
    if (code != 0) {
      if (failed_programs == 0) {
        first_failure = code;
      }
      ++failed_programs;
    }
  }
  envelope.trace = trace.take_events();
  envelope.cache = ctx.cache_outcome;
  envelope.link_sdk_path = ctx.link_sdk_path;
  envelope.status = Status::Ok;
  envelope.outcome = Outcome::Ran;
  if (chosen.is_suite) {
    err.block(i18n::format<i18n::Key::CliRunSuiteSummary>(
                  config.language, run_members.size(), failed_programs) +
              '\n');
  }
  return first_failure;
}

}  // namespace cli
