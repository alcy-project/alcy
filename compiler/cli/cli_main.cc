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
#include "cli/logger.h"
#include "cli/new_command.h"
#include "cli/output.h"
#include "cli/parse_args.h"
#include "cli/parse_output.h"
#include "cli/result_code.h"
#include "cli/run_command.h"
#include "cli/validate.h"
#include "debug/fatal.h"
#include "diag/diagnostic.h"
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
                            const char* const* argv,
                            i18n::Language language) {
  FdSink out_sink{io::STDOUT_FD};
  FdSink err_sink{io::STDERR_FD};
  const Logger out{&FdSink::write, &out_sink};
  const Logger err{&FdSink::write, &err_sink};
  const Interruption interruption =
      render_outcome(parser, outcome, scan_color_mode(argc, argv), language);
  for (const std::string& error : interruption.errors) {
    err.block(error);
  }
  if (!interruption.text.empty()) {
    out.block(interruption.text);
  }
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

  // The help is written before the arguments are parsed, so the language
  // is read off the raw arguments first. A value that names no catalog is
  // the parser's error to report, in the default language.
  const i18n::Language language = scan_language(argc, argv);
  arg::Parser parser = build_parser(language);
  ParseOutcome outcome = parse_args(parser, argc, argv);

  i32 exit_code = result_code(ResultCode::Success);
  if (const std::optional<ResultCode> code = interruption_exit_code(outcome)) {
    exit_code = result_code(
        run_interruption(parser, outcome, *code, argc, argv, language));
  } else {
    const CliConfig& config = outcome.get<CliConfig>();
    FdSink out_sink{io::STDOUT_FD};
    FdSink err_sink{io::STDERR_FD};
    const Logger out{&FdSink::write, &out_sink};
    const Logger err{&FdSink::write, &err_sink};
    // Validation has already rejected `--json` on a verb that has no
    // result document to report, so the flag and the renderer agree.
    const bool json = config.json;
    // Grammar parsing produced the config; semantic validation gates
    // dispatch so no command ever sees a nonsensical field combination.
    base::Result<void, ConfigError> validated = validate_cli_config(config);
    if (validated.is_err()) {
      // A rejected combination is reported as a result rather than a
      // throwaway line, so `--json` covers the failure the same way it
      // covers a build that did not compile. The envelope's diagnostic
      // views this text, so it has to outlive the report.
      const std::string failure = describe_config_error(
          std::move(validated).unwrap_err(), config.language);
      Envelope envelope;
      envelope.command = command_name(config.subcommand);
      envelope.status = Status::Error;
      envelope.failure = diag::message(diag::Severity::Error, failure);
      envelope.wall_ns = elapsed_ns_since(started);
      report(out, err, envelope, config.color_mode, config.language, json);
      exit_code = result_code(ResultCode::ArgParseError);
    } else {
      // One context for the invocation: the command fills it and the
      // envelope borrows it, so the bag and the sources it points at are
      // still alive when the report is rendered.
      pipeline::PipelineContext ctx{config.language};
      Envelope envelope;
      envelope.command = command_name(config.subcommand);
      exit_code = dispatch(config, ctx, envelope);
      envelope.wall_ns = elapsed_ns_since(started);
      report(out, err, envelope, config.color_mode, config.language, json);
    }
  }
  return exit_code;
}

}  // namespace cli
