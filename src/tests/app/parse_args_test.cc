// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#include "app/parse_args.h"

#include <string_view>
#include <utility>

#include "app/driver_config.h"
#include "doctest/doctest.h"
#include "fpag/base/numeric.h"
#include "fpag/term/color_mode.h"
#include "fpag/term/color_style.h"

namespace app {

namespace {

DriverConfig parse_ok(std::string_view argv1,
                      std::string_view argv2 = {},
                      std::string_view argv3 = {}) {
  // String literals are static; views stay valid through the call.
  const char* argv[] = {"alcy", "", "", ""};
  i32 argc = 1;
  const std::string_view rest[] = {argv1, argv2, argv3};
  for (const std::string_view arg : rest) {
    if (arg.empty()) {
      break;
    }
    argv[argc++] = arg.data();
  }
  ParseArgsResult result =
      parse_args(build_parser(), argc, argv, term::ColorStyle::Off);
  CHECK(result.tag() == ParseArgsResult::TagOf<DriverConfig>);
  if (result.tag() != ParseArgsResult::TagOf<DriverConfig>) {
    return DriverConfig{};
  }
  return std::move(result).get<DriverConfig>();
}

}  // namespace

TEST_CASE("Parse build subcommand") {
  const DriverConfig config = parse_ok("build", "--release", "mydir");
  CHECK(config == DriverConfig{.time_trace = false,
                               .color_mode = term::ColorMode::Auto,
                               .subcommand = Subcommand::Build,
                               .release = true,
                               .target_dir = "mydir"});
}

TEST_CASE("Parse build defaults") {
  const DriverConfig config = parse_ok("build");
  CHECK(config.subcommand == Subcommand::Build);
  CHECK(!config.release);
  CHECK(config.target_dir.empty());
}

TEST_CASE("Parse other subcommands") {
  CHECK(parse_ok("test").subcommand == Subcommand::Test);
  CHECK(parse_ok("run").subcommand == Subcommand::Run);
  CHECK(parse_ok("new").subcommand == Subcommand::New);
  CHECK(parse_ok("check").subcommand == Subcommand::Check);
}

TEST_CASE("Parse global flags around subcommands") {
  const DriverConfig before = parse_ok("--color=never", "build");
  CHECK(before.color_mode == term::ColorMode::Never);
  CHECK(before.subcommand == Subcommand::Build);

  const DriverConfig after = parse_ok("build", "--color=never");
  CHECK(after.color_mode == term::ColorMode::Never);
  CHECK(after.subcommand == Subcommand::Build);
}

TEST_CASE("Parse bare invocation requests help") {
  const char* argv[] = {"alcy"};
  ParseArgsResult result =
      parse_args(build_parser(), 1, argv, term::ColorStyle::Off);
  CHECK(result.tag() == ParseArgsResult::TagOf<ParseInterruptedReason>);
  CHECK(std::move(result).get<ParseInterruptedReason>() ==
        ParseInterruptedReason::HelpRequested);
}

TEST_CASE("Parse unknown subcommand") {
  const char* argv[] = {"alcy", "frobnicate"};
  ParseArgsResult result =
      parse_args(build_parser(), 2, argv, term::ColorStyle::Off);
  CHECK(result.tag() == ParseArgsResult::TagOf<ParseInterruptedReason>);
  CHECK(std::move(result).get<ParseInterruptedReason>() ==
        ParseInterruptedReason::UnknownSubcommand);
}

TEST_CASE("Parse unknown flags") {
  const char* argv[] = {"alcy", "build", "--frobnicator"};
  ParseArgsResult result =
      parse_args(build_parser(), 3, argv, term::ColorStyle::Off);
  CHECK(result.tag() == ParseArgsResult::TagOf<ParseInterruptedReason>);
  CHECK(std::move(result).get<ParseInterruptedReason>() ==
        ParseInterruptedReason::ParseError);
}

}  // namespace app
