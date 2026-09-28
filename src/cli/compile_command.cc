// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/compile_command.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/logger.h"
#include "cli/cli_config.h"
#include "cli/diagnostic_output.h"
#include "cli/result_code.h"
#include "cli/trace.h"
#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/io/io_util.h"
#include "pipeline/build.h"
#include "pipeline/pipeline_context.h"
#include "pipeline/std_select.h"
#include "source/source.h"

namespace cli {

namespace {

// The name a program arriving on a pipe is reported under. A pipe carries
// no file behind it, so this is the honest label rather than a stand-in
// for a path the user could open.
constexpr std::string_view STDIN_NAME = "<stdin>";

// Reads standard input to its end. A pipe hands over whatever is ready, so
// the read count is the loop's condition and only a negative one is an
// error.
base::Result<std::string, std::string_view> read_stdin() {
  std::string text;
  // Doubling until the end: a program is not known to be small, and one
  // read cannot say how much is left.
  constexpr usize CHUNK = static_cast<usize>(64) * 1024;
  std::vector<char> buffer(CHUNK);
  while (true) {
    const isize got = io::read(io::STDIN_FD, buffer.data(), buffer.size());
    if (got < 0) {
      return base::make_err(std::string_view("cannot read standard input"));
    }
    if (got == 0) {
      return base::make_ok(std::move(text));
    }
    text.append(buffer.data(), static_cast<usize>(got));
  }
}

}  // namespace

ResultCode run_compile(const CliConfig& config,
                       const diag::RenderOptions& options) {
  pipeline::PipelineContext ctx;
  TraceSession trace(ctx, config.time_trace);
  // Validation guarantees one of these: a target, or the pipe with a
  // named output.
  if (config.stdin_source) {
    trace.set_path(trace_path_beside(config.output));
    base::Result<std::string, std::string_view> text = read_stdin();
    if (text.is_err()) {
      const u32 index =
          ctx.bag.emit(diag::Severity::Error, pipeline::PIPELINE_IO_ERROR,
                       "cannot read standard input");
      (void)index;
      report_diagnostics(ctx.bag, ctx.sources, options);
      return ResultCode::BuildFailed;
    }
    // The manager copies the text, so the buffer can go straight after.
    const source::FileId root =
        ctx.sources.add_virtual(STDIN_NAME, std::move(text).unwrap());
    base::Result<void, diag::Reported> res = pipeline::build_single_root(
        ctx, root, config.output, config.release, config.linker, config.emit,
        pipeline::full_std_selection());
    if (!trace.finish()) {
      const u32 index =
          ctx.bag.emit(diag::Severity::Error, pipeline::PIPELINE_IO_ERROR,
                       "cannot write trace '{}'", trace.path());
      (void)index;
    }
    report_diagnostics(ctx.bag, ctx.sources, options);
    if (res.is_err() || ctx.bag.has_errors()) {
      return ResultCode::BuildFailed;
    }
    base::logger.wo_prefix("compiled successfully");
    return ResultCode::Success;
  }

  trace.set_path(trace_path_beside(config.output));
  base::Result<void, diag::Reported> res = pipeline::build_single_file(
      ctx, config.target_dir, config.output, config.release, config.linker,
      config.emit, pipeline::full_std_selection());
  if (!trace.finish()) {
    const u32 index =
        ctx.bag.emit(diag::Severity::Error, pipeline::PIPELINE_IO_ERROR,
                     "cannot write trace '{}'", trace.path());
    (void)index;
  }
  // Report before branching: a successful compile still carries the
  // warnings the bag collected along the way.
  report_diagnostics(ctx.bag, ctx.sources, options);
  if (res.is_err() || ctx.bag.has_errors()) {
    return ResultCode::BuildFailed;
  }
  base::logger.wo_prefix("compiled successfully");
  return ResultCode::Success;
}

}  // namespace cli
