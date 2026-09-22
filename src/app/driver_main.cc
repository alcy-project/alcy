// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "app/driver_main.h"

#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

#include "app/build_command.h"
#include "app/check_command.h"
#include "app/driver_config.h"
#include "app/driver_context.h"
#include "app/init_handler.h"
#include "app/new_command.h"
#include "app/parse_args.h"
#include "app/parse_output.h"
#include "app/result_code.h"
#include "base/logger.h"
#include "debug/fatal.h"
#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "fpag/arg/parser.h"
#include "fpag/base/numeric.h"
#include "fpag/term/color_style.h"
#include "fpag/term/console.h"

namespace app {

namespace {

ResultCode not_implemented(std::string_view subcommand) {
  DriverContext ctx;
  const u32 index =
      ctx.bag.emit(diag::Severity::Error, kDriverNotImplemented,
                   "'alcy {}' is not implemented yet", subcommand);
  (void)index;
  report(ctx.bag, ctx.sources);
  return ResultCode::NotImplemented;
}

ResultCode dispatch(const DriverConfig& config) {
  switch (config.subcommand) {
    case Subcommand::Build: return run_build(config);
    case Subcommand::New: return run_new(config.target_dir);
    case Subcommand::Test: return not_implemented("test");
    case Subcommand::Run: return not_implemented("run");
    case Subcommand::Check: return run_check(config);
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

i32 driver_main(i32 argc, char** argv) {
  init_runtime();

  arg::Parser parser = build_parser();
  ParseOutcome outcome = parse_args(parser, argc, argv);

  ResultCode result = ResultCode::Success;
  if (const std::optional<ResultCode> code = interruption_exit_code(outcome)) {
    result = run_interruption(parser, outcome, *code, argc, argv);
  } else {
    const DriverConfig& config = outcome.get<DriverConfig>();
    base::init_logger(
        term::console_color_style(term::Stream::Stdout, config.color_mode));
    result = dispatch(config);
  }
  return result_code(result);
}

}  // namespace app
