// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/compile_command.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cli/cli_config.h"
#include "cli/output.h"
#include "cli/result_code.h"
#include "cli/trace.h"
#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/io/io_util.h"
#include "i18n/messages.h"
#include "pipeline/build.h"
#include "pipeline/link_options.h"
#include "pipeline/pipeline_context.h"
#include "pipeline/std_select.h"
#include "pkg/manifest.h"
#include "pkg/toolchain.h"
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

// The standard library a single file sees: the default suite, or
// nothing with `--no-std`, plus every `--deps` fragment. Fragments parse
// with the manifest grammar, so a typo fails the same way in both.
base::Result<pipeline::StdSelection, diag::Reported> compile_selection(
    const CliConfig& config,
    pipeline::PipelineContext& ctx) {
  std::vector<pkg::Dependency> deps;
  if (!config.no_std) {
    base::Result<pkg::Dependency, diag::Reported> suite =
        pkg::parse_dependency_flag(ctx.bag, ctx.arena, "alcy/std/*");
    if (suite.is_err()) {
      return base::make_err(diag::Reported{});
    }
    deps.push_back(std::move(suite).unwrap());
  }
  for (std::string_view fragment : config.deps) {
    base::Result<pkg::Dependency, diag::Reported> parsed =
        pkg::parse_dependency_flag(ctx.bag, ctx.arena, fragment);
    if (parsed.is_err()) {
      return base::make_err(diag::Reported{});
    }
    deps.push_back(std::move(parsed).unwrap());
  }
  return pipeline::resolve_std_selection(deps, ctx.bag);
}

// What was written is what the reader wants to know after a compile. The
// pipeline resolved the path, so the report names the file that exists
// and measures that, rather than leaving the reader to guess which of
// the requested and the written differ.
void record_output(const std::string& output, Envelope& envelope) {
  envelope.output_path = output;
  const isize size = io::file_size(output);
  if (size > 0) {
    envelope.output_bytes = static_cast<u64>(size);
  }
}

ResultCode run_compile(const CliConfig& config,
                       pipeline::PipelineContext& ctx,
                       Envelope& envelope) {
  TraceSession trace(ctx, config.time_trace);
  envelope.bag = &ctx.bag;
  envelope.sources = &ctx.sources;
  const ResultCode failed = ResultCode::BuildFailed;
  base::Result<pipeline::StdSelection, diag::Reported> selected =
      compile_selection(config, ctx);
  if (selected.is_err() || ctx.bag.has_errors()) {
    return failed;
  }
  const pipeline::StdSelection selection = std::move(selected).unwrap();
  // `compile` never reads a toolchain file, so one file stays
  // reproducible from the command alone; see docs/adr/0019.
  const pipeline::LinkOptions link =
      resolve_link_options(config, pkg::Toolchain{});
  // Validation guarantees one of these: a target, or the pipe with a
  // named output.
  if (config.stdin_source) {
    base::Result<std::string, std::string_view> text = read_stdin();
    if (text.is_err()) {
      const u32 index =
          ctx.bag.emit<i18n::Key::PipelineCannotReadStandardInput>(
              diag::Severity::Error, pipeline::PIPELINE_IO_ERROR);
      (void)index;
      return failed;
    }
    // The manager copies the text, so the buffer can go straight after.
    const source::FileId root =
        ctx.sources.add_virtual(STDIN_NAME, std::move(text).unwrap());
    base::Result<std::string, diag::Reported> res = pipeline::build_single_root(
        ctx, root, config.output, config.release, link, config.emit, selection);
    envelope.trace = trace.take_events();
    if (res.is_err() || ctx.bag.has_errors()) {
      return failed;
    }
    envelope.status = Status::Ok;
    envelope.outcome = Outcome::Compiled;
    record_output(std::move(res).unwrap(), envelope);
    return ResultCode::Success;
  }

  base::Result<std::string, diag::Reported> res =
      pipeline::build_single_file(ctx, config.target_dir, config.output,
                                  config.release, link, config.emit, selection);
  envelope.trace = trace.take_events();
  if (res.is_err() || ctx.bag.has_errors()) {
    return failed;
  }
  envelope.status = Status::Ok;
  envelope.outcome = Outcome::Compiled;
  record_output(std::move(res).unwrap(), envelope);
  return ResultCode::Success;
}

}  // namespace cli
