// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/cli_main.h"

#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

#include "base/logger.h"
#include "cli/build_command.h"
#include "cli/check_command.h"
#include "cli/cli_config.h"
#include "cli/init_command.h"
#include "cli/init_handler.h"
#include "cli/new_command.h"
#include "cli/parse_args.h"
#include "cli/parse_output.h"
#include "cli/result_code.h"
#include "cli/run_command.h"
#include "debug/fatal.h"
#include "fpag/arg/parser.h"
#include "fpag/base/numeric.h"
#include "fpag/term/color_style.h"
#include "fpag/term/console.h"

namespace cli {

namespace {

i32 dispatch(const CliConfig& config) {
  switch (config.subcommand) {
    case Subcommand::Build: return result_code(run_build(config));
    case Subcommand::Run: return run_run(config);
    case Subcommand::New: return result_code(run_new(config.target_dir));
    case Subcommand::Init: return result_code(run_init(config));
    case Subcommand::Check: return result_code(run_check(config));
    case Subcommand::None: break;
  }
  // parse_args maps a missing subcommand to NoSubcommand/UnknownSubcommand,
  // so only interruption outcomes carry Subcommand::None here.
  UNREACHABLE();
}

ResultCode run_interruption(arg::Parser& parser,
                            const ParseOutcome& outcome,
                            ResultCode code,
                            i32 argc,
                            const char* const* argv) {
  const term::ColorStyle style = term::console_color_style(
      term::Stream::Stdout, scan_color_mode(argc, argv));
  base::init_logger(style);
  const std::string text = render_outcome(parser, outcome, style);
  if (!text.empty()) {
    base::logger.wo_prefix("{}", text);
  }
  return code;
}

}  // namespace

i32 cli_main(i32 argc, char** argv) {
  init_runtime();

  arg::Parser parser = build_parser();
  ParseOutcome outcome = parse_args(parser, argc, argv);

  i32 exit_code = result_code(ResultCode::Success);
  if (const std::optional<ResultCode> code = interruption_exit_code(outcome)) {
    exit_code =
        result_code(run_interruption(parser, outcome, *code, argc, argv));
  } else {
    const CliConfig& config = outcome.get<CliConfig>();
    base::init_logger(
        term::console_color_style(term::Stream::Stdout, config.color_mode));
    exit_code = dispatch(config);
  }
  return exit_code;
}

}  // namespace cli
