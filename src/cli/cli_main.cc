// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/cli_main.h"

#include <chrono>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "cli/build_command.h"
#include "cli/check_command.h"
#include "cli/cli_config.h"
#include "cli/compile_command.h"
#include "cli/init_command.h"
#include "cli/init_handler.h"
#include "cli/new_command.h"
#include "cli/output.h"
#include "cli/parse_args.h"
#include "cli/parse_output.h"
#include "cli/result_code.h"
#include "cli/run_command.h"
#include "cli/validate.h"
#include "debug/fatal.h"
#include "diag/render.h"
#include "fpag/arg/parser.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/io/io_util.h"
#include "fpag/term/color_style.h"
#include "fpag/term/console.h"
#include "i18n/language.h"
#include "pipeline/pipeline_context.h"

namespace cli {

namespace {

i32 dispatch(const CliConfig& config,
             pipeline::PipelineContext& ctx,
             Envelope& envelope) {
  switch (config.subcommand) {
    case Subcommand::Build:
      return result_code(run_build(config, ctx, envelope));
    case Subcommand::Compile:
      return result_code(run_compile(config, ctx, envelope));
    case Subcommand::Run: return run_run(config, ctx, envelope);
    case Subcommand::New: return result_code(run_new(config, ctx, envelope));
    case Subcommand::Init: return result_code(run_init(config, ctx, envelope));
    case Subcommand::Check:
      return result_code(run_check(config, ctx, envelope));
    case Subcommand::None: break;
  }
  // parse_args maps a missing subcommand to NoSubcommand/UnknownSubcommand,
  // so only interruption outcomes carry Subcommand::None here.
  UNREACHABLE();
}

void write_stdout(std::string_view text) {
  if (text.empty()) {
    return;
  }
  io::write(io::STDOUT_FD, text.data(), text.size());
}

// The verb as spelled on the command line, which is also what the
// envelope reports as its command.
std::string_view command_name(Subcommand subcommand) {
  switch (subcommand) {
    case Subcommand::Build: return "build";
    case Subcommand::Compile: return "compile";
    case Subcommand::Run: return "run";
    case Subcommand::New: return "new";
    case Subcommand::Init: return "init";
    case Subcommand::Check: return "check";
    case Subcommand::None: break;
  }
  UNREACHABLE();
}

ResultCode run_interruption(const arg::Parser& parser,
                            const ParseOutcome& outcome,
                            ResultCode code,
                            i32 argc,
                            const char* const* argv) {
  const term::ColorStyle style = term::console_color_style(
      term::Stream::Stdout, scan_color_mode(argc, argv));
  write_stdout(render_outcome(parser, outcome, style));
  return code;
}

}  // namespace

i32 cli_main(i32 argc, char** argv) {
  init_runtime();
  // Measured here so the envelope reports the whole invocation, argument
  // parsing and terminal detection included. The profiler's own clock is
  // wall-clock derived and is not used for it.
  const std::chrono::steady_clock::time_point started =
      std::chrono::steady_clock::now();

  arg::Parser parser = build_parser();
  ParseOutcome outcome = parse_args(parser, argc, argv);

  i32 exit_code = result_code(ResultCode::Success);
  if (const std::optional<ResultCode> code = interruption_exit_code(outcome)) {
    exit_code =
        result_code(run_interruption(parser, outcome, *code, argc, argv));
  } else {
    const CliConfig& config = outcome.get<CliConfig>();
    const term::ColorStyle style =
        term::console_color_style(term::Stream::Stdout, config.color_mode);
    const diag::RenderOptions options{
        .color = style != term::ColorStyle::Off,
    };
    // Validation has already rejected `--json` on a verb that has no
    // result document to report, so the flag and the renderer agree.
    const bool json = config.json;
    // Grammar parsing produced the config; semantic validation gates
    // dispatch so no command ever sees a nonsensical field combination.
    base::Result<void, ConfigError> validated = validate_cli_config(config);
    if (validated.is_err()) {
      // A rejected combination is reported as a result rather than a
      // throwaway line, so `--json` covers the failure the same way it
      // covers a build that did not compile.
      Envelope envelope;
      envelope.command = command_name(config.subcommand);
      envelope.status = Status::Error;
      envelope.failure =
          describe_config_error(std::move(validated).unwrap_err());
      envelope.wall_ns = elapsed_ns_since(started);
      report(envelope, options, json);
      exit_code = result_code(ResultCode::ArgParseError);
    } else {
      // One context for the invocation: the command fills it and the
      // envelope borrows it, so the bag and the sources it points at are
      // still alive when the report is rendered.
      pipeline::PipelineContext ctx{i18n::Language::EnUs};
      Envelope envelope;
      envelope.command = command_name(config.subcommand);
      exit_code = dispatch(config, ctx, envelope);
      envelope.wall_ns = elapsed_ns_since(started);
      report(envelope, options, json);
    }
  }
  return exit_code;
}

}  // namespace cli
