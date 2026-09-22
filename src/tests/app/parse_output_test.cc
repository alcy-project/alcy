// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "app/parse_output.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "app/driver_config.h"
#include "app/parse_args.h"
#include "app/result_code.h"
#include "doctest/doctest.h"
#include "fpag/arg/parser.h"
#include "fpag/term/color_mode.h"
#include "fpag/term/color_style.h"

namespace app {

namespace {

ParseOutcome parse(std::span<const std::string_view> args) {
  arg::Parser parser = build_parser();
  return parse_args(parser, args);
}

std::string render(ParseOutcome&& outcome) {
  arg::Parser parser = build_parser();
  return render_outcome(parser, outcome, term::ColorStyle::Off);
}

}  // namespace

TEST_CASE("Render help for explicit and bare invocations") {
  const std::string_view help[] = {"alcy", "--help"};
  const std::string help_text = render(parse(help));
  CHECK(!help_text.empty());
  CHECK(help_text.find("build") != std::string::npos);

  const std::string_view bare[] = {"alcy"};
  const std::string bare_text = render(parse(bare));
  CHECK(!bare_text.empty());
  CHECK(bare_text.find("build") != std::string::npos);
}

TEST_CASE("Render version") {
  const std::string_view args[] = {"alcy", "--version"};
  arg::Parser parser = build_parser();
  ParseOutcome outcome = parse(args);
  const std::string text =
      render_outcome(parser, outcome, term::ColorStyle::Off);
  CHECK(!text.empty());
  CHECK(text.find(std::string(parser.root_command().version())) !=
        std::string::npos);
}

TEST_CASE("Render unknown subcommand") {
  const std::string_view args[] = {"alcy", "frobnicate"};
  const std::string text = render(parse(args));
  CHECK(text.find("frobnicate") != std::string::npos);
  CHECK(text.find("--help") != std::string::npos);
}

TEST_CASE("Render parse failure") {
  const std::string_view args[] = {"alcy", "build", "--frobnicator"};
  const std::string text = render(parse(args));
  CHECK(!text.empty());
}

TEST_CASE("Render config is empty") {
  const std::string_view args[] = {"alcy", "build"};
  CHECK(render(parse(args)).empty());
}

TEST_CASE("Interruption exit codes") {
  const std::string_view build[] = {"alcy", "build"};
  const std::string_view bare[] = {"alcy"};
  const std::string_view help[] = {"alcy", "--help"};
  const std::string_view version[] = {"alcy", "--version"};
  const std::string_view unknown[] = {"alcy", "frobnicate"};
  const std::string_view broken[] = {"alcy", "build", "--frobnicator"};

  CHECK(parse(build).is<DriverConfig>());
  CHECK(interruption_exit_code(parse(build)) == std::nullopt);
  CHECK(interruption_exit_code(parse(bare)) == ResultCode::Success);
  CHECK(interruption_exit_code(parse(help)) == ResultCode::Success);
  CHECK(interruption_exit_code(parse(version)) == ResultCode::Success);
  CHECK(interruption_exit_code(parse(unknown)) == ResultCode::ArgParseError);
  CHECK(interruption_exit_code(parse(broken)) == ResultCode::ArgParseError);
}

TEST_CASE("Scan color mode") {
  const char* none[] = {"alcy", "build"};
  const char* equals[] = {"alcy", "--color=never", "build"};
  const char* separate[] = {"alcy", "build", "--color", "always"};
  const char* bogus[] = {"alcy", "--color=bogus", "build"};
  CHECK(scan_color_mode(2, none) == term::ColorMode::Auto);
  CHECK(scan_color_mode(3, equals) == term::ColorMode::Never);
  CHECK(scan_color_mode(4, separate) == term::ColorMode::Always);
  CHECK(scan_color_mode(3, bogus) == term::ColorMode::Auto);
  CHECK(scan_color_mode(0, nullptr) == term::ColorMode::Auto);
}

}  // namespace app
