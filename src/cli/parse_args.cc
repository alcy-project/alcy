// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/parse_args.h"

#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "cli/cli_config.h"
#include "cli/converters.h"  // IWYU pragma: keep
#include "debug/fatal.h"
#include "fpag/arg/arg.h"
#include "fpag/arg/command.h"
#include "fpag/arg/matches.h"
#include "fpag/arg/parse_result.h"
#include "fpag/arg/parse_status.h"
#include "fpag/arg/parser.h"
#include "fpag/base/numeric.h"
#include "fpag/term/color_mode.h"

namespace cli {

namespace {

CliConfig extract_from_matches(arg::Matches&& matches) {
  CliConfig c{};
  c.time_trace = matches.get<bool>("time-trace").unwrap_or(c.time_trace);
  c.color_mode = matches.get<term::ColorMode>("color").unwrap_or(c.color_mode);

  const std::string_view selected = matches.selected_command();
  if (selected == "build") {
    c.subcommand = Subcommand::Build;
  } else if (selected == "run") {
    c.subcommand = Subcommand::Run;
  } else if (selected == "new") {
    c.subcommand = Subcommand::New;
  } else if (selected == "init") {
    c.subcommand = Subcommand::Init;
  } else if (selected == "check") {
    c.subcommand = Subcommand::Check;
  }
  c.release = matches.get<bool>("release").unwrap_or(false);
  c.output = matches.get<std::string_view>("output").unwrap_or(c.output);
  c.linker = matches.get<std::string_view>("linker").unwrap_or(c.linker);

  const std::span<const std::string_view> positionals = matches.positionals();
  if (!positionals.empty()) {
    c.target_dir = positionals[0];
    c.program_args.assign(positionals.begin() + 1, positionals.end());
  }

  return c;
}

ParseOutcome to_outcome(arg::ParseResult<arg::Matches>&& result) {
  switch (result.status()) {
    case arg::ParseStatus::Success: {
      CliConfig config = extract_from_matches(std::move(result).unwrap());
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
          .add_arg(arg::ArgBuilder("output")
                       .short_name('o')
                       .help("Object output path for single-file builds.")
                       .default_value("")
                       .build())
          .add_arg(arg::ArgBuilder("linker")
                       .help("System linker driver for executable builds.")
                       .default_value("")
                       .build())
          .build());
  builder.add_subcommand(
      build_subcommand("run", "Build and run a package or source file")
          .add_arg(arg::ArgBuilder("release")
                       .help("Build with optimizations.")
                       .is_flag(true)
                       .build())
          .add_arg(arg::ArgBuilder("linker")
                       .help("System linker driver for executable builds.")
                       .default_value("")
                       .build())
          .build());
  builder.add_subcommand(
      build_subcommand("new", "Create a new package").build());
  builder.add_subcommand(
      build_subcommand("init", "Create a package in an existing directory")
          .build());
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

}  // namespace cli
