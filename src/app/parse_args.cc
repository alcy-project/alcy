// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#include "app/parse_args.h"

#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "app/converters.h"  // IWYU pragma: keep
#include "app/driver_config.h"
#include "debug/fatal.h"
#include "fpag/arg/arg.h"
#include "fpag/arg/command.h"
#include "fpag/arg/matches.h"
#include "fpag/arg/parse_result.h"
#include "fpag/arg/parse_status.h"
#include "fpag/arg/parser.h"
#include "fpag/base/numeric.h"
#include "fpag/term/color_mode.h"

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

  const std::span<const std::string_view> positionals = matches.positionals();
  if (!positionals.empty()) {
    c.target_dir = positionals[0];
  }

  return c;
}

ParseOutcome to_outcome(arg::ParseResult<arg::Matches>&& result) {
  switch (result.status()) {
    case arg::ParseStatus::Success: {
      DriverConfig config = extract_from_matches(std::move(result).unwrap());
      if (config.subcommand == Subcommand::None) {
        if (!config.target_dir.empty()) {
          return UnknownSubcommand{std::string(config.target_dir)};
        }
        return NoSubcommand{};
      }
      return config;
    }
    case arg::ParseStatus::Error:
      return ParseFailure{std::move(result).unwrap_err()};
    case arg::ParseStatus::HelpRequested: return HelpRequested{};
    case arg::ParseStatus::VersionRequested: return VersionRequested{};
    default: UNREACHABLE();
  }
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

ParseOutcome parse_args(arg::Parser& parser,
                        std::span<const std::string_view> args) {
  return to_outcome(parser.try_parse(args));
}

ParseOutcome parse_args(arg::Parser& parser,
                        i32 argc,
                        const char* const* argv) {
  return to_outcome(parser.try_parse(argc, argv));
}

}  // namespace app
