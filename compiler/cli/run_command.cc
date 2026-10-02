// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/run_command.h"

#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "cli/cli_config.h"
#include "cli/logger.h"
#include "cli/output.h"
#include "cli/result_code.h"
#include "cli/trace.h"
#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "diag/render.h"
#include "diag/stage.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/io/io_util.h"
#include "fpag/term/color_style.h"
#include "fpag/term/console.h"
#include "i18n/messages.h"
#include "path/path.h"
#include "pipeline/diag_code.h"
#include "pipeline/link_options.h"
#include "pipeline/pipeline_context.h"
#include "pipeline/run.h"
#include "pipeline/target.h"
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
};

void announce_exec(std::string_view target, const void* ctx) {
  const auto& a = *static_cast<const Announcement*>(ctx);
  announce(a.err, "Running", target, a.options);
}

}  // namespace

i32 run_run(const CliConfig& config,
            pipeline::PipelineContext& ctx,
            Envelope& envelope) {
  TraceSession trace(ctx, config.time_trace);
  envelope.bag = &ctx.bag;
  envelope.sources = &ctx.sources;
  const i32 failed = result_code(ResultCode::RunFailed);
  // Validation keeps single files out: `run` takes a package directory,
  // and `compile` takes the file.
  const std::string_view raw_dir =
      config.target_dir.empty() ? "." : config.target_dir;
  const std::span<const std::string_view> args(config.program_args);

  base::Result<pipeline::ManifestProbe, path::PathError> probe =
      pipeline::find_package_manifest(ctx, raw_dir);
  if (probe.is_err()) {
    return failed;
  }
  pipeline::ManifestProbe found = std::move(probe).unwrap();
  if (!found.found) {
    const u32 index = ctx.bag.emit<i18n::Key::PipelineNoManifest>(
        diag::Severity::Error, diag::Stage::Pipeline,
        pipeline::DiagCode::NoManifest, raw_dir);
    (void)index;
    return failed;
  }
  base::Result<pkg::Toolchain, diag::Reported> toolchain =
      pipeline::load_toolchain(ctx, found.root);
  if (toolchain.is_err()) {
    return failed;
  }
  const pkg::Toolchain tool = std::move(toolchain).unwrap();
  const pipeline::LinkOptions link = resolve_link_options(config, tool);
  const term::ColorStyle style =
      term::console_color_style(term::Stream::Stderr, config.color_mode);
  const diag::RenderOptions announce_options{
      .color = style != term::ColorStyle::Off,
  };
  FdSink err_sink{io::STDERR_FD};
  const Logger err{&FdSink::write, &err_sink};
  const Announcement announcement{announce_options, err};
  base::Result<pipeline::RunOutcome, diag::Reported> result =
      pipeline::run_package(ctx, found.root, found.manifest,
                            found.manifest_name, config.release, link, args,
                            announce_exec, &announcement);
  envelope.trace = trace.take_events();
  if (result.is_err()) {
    return failed;
  }
  // A run that reaches the program still carries the warnings the bag
  // collected along the way, so the report is not a success-only line.
  envelope.status = Status::Ok;
  envelope.outcome = Outcome::Ran;
  return std::move(result).unwrap().exit_code;
}

}  // namespace cli
