// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#include "app/parse_args.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "app/converters.h"  // IWYU pragma: keep
#include "app/driver_config.h"
#include "base/logger.h"
#include "debug/fatal.h"
#include "fpag/arg/arg.h"
#include "fpag/arg/command.h"
#include "fpag/arg/error_formatter.h"
#include "fpag/arg/help_formatter.h"
#include "fpag/arg/matches.h"
#include "fpag/arg/parse_error.h"
#include "fpag/arg/parse_result.h"
#include "fpag/arg/parse_status.h"
#include "fpag/arg/parser.h"
#include "fpag/arg/version_formatter.h"
#include "fpag/base/numeric.h"
#include "fpag/term/color_mode.h"
#include "fpag/term/color_style.h"

namespace app {

namespace {

DriverConfig extract_from_matches(arg::Matches&& matches) {
  DriverConfig c{};
  c.time_trace = matches.get<bool>("time-trace").unwrap_or(c.time_trace);
  c.color_mode = matches.get<term::ColorMode>("color").unwrap_or(c.color_mode);

  const std::string_view selected = matches.selected_command();
  if (selected == "build") {
    c.subcommand = Subcommand::Build;
  } else if (selected == "test") {
    c.subcommand = Subcommand::Test;
  } else if (selected == "run") {
    c.subcommand = Subcommand::Run;
  } else if (selected == "new") {
    c.subcommand = Subcommand::New;
  } else if (selected == "check") {
    c.subcommand = Subcommand::Check;
  }
  c.release = matches.get<bool>("release").unwrap_or(false);

  const std::vector<std::string_view>& positionals = matches.positionals();
  if (!positionals.empty()) {
    c.target_dir = positionals[0];
  }

  return c;
}

arg::CommandBuilder build_subcommand(std::string name, std::string about) {
  arg::CommandBuilder builder(std::move(name));
  builder.about(std::move(about));
  return builder;
}

}  // namespace

arg::Parser build_parser() {
  arg::CommandBuilder builder(ALCY_PROJECT_NAME, ALCY_PROJECT_VERSION);
  builder.about(ALCY_COMMAND_ABOUT);
  builder.builtin_enabled(true);
  builder.add_arg(arg::ArgBuilder("color")
                      .help("Color mode for logging")
                      .default_value("auto")
                      .choices({"auto", "always", "never"})
                      .build());
  builder.add_arg(arg::ArgBuilder("time-trace")
                      .short_name('t')
                      .help("Enable time profiling and generate the json file.")
                      .is_flag(true)
                      .build());
  builder.add_subcommand(
      build_subcommand("build", "Build a package or source directory")
          .add_arg(arg::ArgBuilder("release")
                       .help("Build with optimizations.")
                       .is_flag(true)
                       .build())
          .build());
  builder.add_subcommand(build_subcommand("test", "Run tests").build());
  builder.add_subcommand(build_subcommand("run", "Run a package").build());
  builder.add_subcommand(
      build_subcommand("new", "Create a new package").build());
  builder.add_subcommand(
      build_subcommand("check", "Check a package without emitting code")
          .build());
  return arg::Parser(std::move(builder).build());
}

ParseArgsResult parse_args(arg::Parser&& parser,
                           i32 argc,
                           const char* const* argv,
                           term::ColorStyle style) {
  const std::string_view name = parser.root_command().name();
  arg::ParseResult<arg::Matches> result = parser.try_parse(argc, argv);

  switch (result.status()) {
    case arg::ParseStatus::Success: {
      DriverConfig config = extract_from_matches(std::move(result).unwrap());
      if (config.subcommand == Subcommand::None) {
        if (!config.target_dir.empty()) {
          base::logger.wo_prefix("unknown subcommand '{}'; see '{} --help'",
                                 config.target_dir, name);
          return ParseInterruptedReason::UnknownSubcommand;
        }
        base::logger.wo_prefix(
            "{}", parser.help_message(arg::DefaultHelpFormatter{}, style));
        return ParseInterruptedReason::HelpRequested;
      }
      return config;
    }
    case arg::ParseStatus::Error: {
      std::vector<arg::ParseError>&& errors = std::move(result).unwrap_err();
      const arg::DefaultErrorFormatter f;
      base::logger.wo_prefix("{}", f(name, errors, style));
      return ParseInterruptedReason::ParseError;
    }
    case arg::ParseStatus::HelpRequested: {
      std::string&& help = std::move(result).unwrap_help();
      base::logger.wo_prefix("{}", std::move(help));
      return ParseInterruptedReason::HelpRequested;
    }
    case arg::ParseStatus::VersionRequested: {
      std::string&& version = std::move(result).unwrap_version();
      const arg::DefaultVersionFormatter f;
      base::logger.wo_prefix("{}", f(name, std::move(version), style));
      return ParseInterruptedReason::VersionRequested;
    }
    default: UNREACHABLE();
  }
}

}  // namespace app
