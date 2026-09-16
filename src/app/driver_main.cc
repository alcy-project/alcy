// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#include "app/driver_main.h"

#include <utility>

#include "app/driver_config.h"
#include "app/init_handler.h"
#include "app/parse_args.h"
#include "app/result_code.h"
#include "debug/fatal.h"
#include "fpag/arg/parser.h"
#include "fpag/base/numeric.h"
#include "fpag/term/color_mode.h"
#include "fpag/term/console.h"

namespace app {

namespace {

i32 dispatch(ParseArgsResult&& args_result) {
  switch (args_result.tag()) {
    case ParseArgsResult::TagOf<DriverConfig>: {
      const DriverConfig config = std::move(args_result).get<DriverConfig>();
      (void)(config);
      // TODO
      return result_code(ResultCode::Success);
    }
    case ParseArgsResult::TagOf<ParseInterruptedReason>: {
      switch (std::move(args_result).get<ParseInterruptedReason>()) {
        case ParseInterruptedReason::ParseError: {
          return result_code(ResultCode::ArgParseError);
        }
        case ParseInterruptedReason::HelpRequested: {
          return result_code(ResultCode::Success);
        }
        case ParseInterruptedReason::VersionRequested: {
          return result_code(ResultCode::Success);
        }
      }
    }
    default: UNREACHABLE();
  }
}

}  // namespace

i32 driver_main(i32 argc, char** argv) {
  init();

  arg::Parser parser = build_parser();
  ParseArgsResult args_result = parse_args(
      std::move(parser), argc, argv,
      term::console_color_style(term::Stream::Stdout, term::ColorMode::Auto));
  return dispatch(std::move(args_result));
}

}  // namespace app
