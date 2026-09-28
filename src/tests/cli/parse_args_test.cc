// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/parse_args.h"

#include <span>
#include <string_view>
#include <utility>

#include "cli/cli_config.h"
#include "doctest/doctest.h"
#include "fpag/arg/parser.h"
#include "fpag/term/color_mode.h"
#include "pipeline/emit_mode.h"

namespace cli {

namespace {

ParseOutcome parse(std::span<const std::string_view> args) {
  arg::Parser parser = build_parser();
  return parse_args(parser, args);
}

CliConfig parse_ok(std::span<const std::string_view> args) {
  ParseOutcome outcome = parse(args);
  CHECK(outcome.is<CliConfig>());
  if (!outcome.is<CliConfig>()) {
    return CliConfig{};
  }
  return outcome.get<CliConfig>();
}

}  // namespace

TEST_CASE("Parse build subcommand") {
  const std::string_view args[] = {"alcy", "build", "--release", "mydir"};
  const CliConfig config = parse_ok(args);
  CHECK(config == CliConfig{.time_trace = false,
                            .color_mode = term::ColorMode::Auto,
                            .subcommand = Subcommand::Build,
                            .release = true,
                            .target_dir = "mydir",
                            .file = "",
                            .output = "",
                            .linker = "",
                            .program_args = {}});
}

TEST_CASE("Parse build output flag") {
  const std::string_view args[] = {"alcy", "build", "main.al", "-o", "main.o"};
  const CliConfig config = parse_ok(args);
  CHECK(config.subcommand == Subcommand::Build);
  CHECK(config.target_dir == "main.al");
  CHECK(config.output == "main.o");
}

TEST_CASE("Parse build linker flag") {
  const std::string_view args[] = {"alcy", "build", "--linker", "clang++"};
  const CliConfig config = parse_ok(args);
  CHECK(config.subcommand == Subcommand::Build);
  CHECK(config.linker == "clang++");
}

TEST_CASE("Parse emit modes") {
  const std::string_view exe[] = {"alcy", "build", "--emit=executable"};
  CHECK(parse_ok(exe).emit == pipeline::EmitMode::Executable);
  const std::string_view obj[] = {"alcy", "build", "--emit=object"};
  CHECK(parse_ok(obj).emit == pipeline::EmitMode::Object);
  const std::string_view llvm[] = {"alcy", "build", "--emit=llvm-ir"};
  CHECK(parse_ok(llvm).emit == pipeline::EmitMode::LlvmIr);
}

TEST_CASE("Parse rejects the reserved ir spelling") {
  // The argument's choices gate first; the converter is the second line
  // of defence. `ir` names alcy's own IR, so it must not spell LLVM IR.
  const std::string_view ir[] = {"alcy", "build", "--emit=ir"};
  CHECK(parse(ir).is<ParseFailure>());
  const std::string_view exe[] = {"alcy", "build", "--emit=exe"};
  CHECK(parse(exe).is<ParseFailure>());
  const std::string_view bitcode[] = {"alcy", "build", "--emit=bitcode"};
  CHECK(parse(bitcode).is<ParseFailure>());
}

TEST_CASE("Parse build defaults") {
  const std::string_view args[] = {"alcy", "build"};
  const CliConfig config = parse_ok(args);
  CHECK(config.subcommand == Subcommand::Build);
  CHECK(!config.release);
  CHECK(config.target_dir.empty());
}

TEST_CASE("Parse other subcommands") {
  const std::string_view run[] = {"alcy", "run"};
  const std::string_view created[] = {"alcy", "new", "mypkg"};
  const std::string_view init[] = {"alcy", "init", "existing"};
  const std::string_view check[] = {"alcy", "check"};
  const std::string_view compile[] = {"alcy", "compile", "main.al"};
  CHECK(parse_ok(run).subcommand == Subcommand::Run);
  CHECK(parse_ok(created).subcommand == Subcommand::New);
  CHECK(parse_ok(created).target_dir == "mypkg");
  CHECK(parse_ok(init).subcommand == Subcommand::Init);
  CHECK(parse_ok(init).target_dir == "existing");
  CHECK(parse_ok(check).subcommand == Subcommand::Check);
  CHECK(parse_ok(compile).subcommand == Subcommand::Compile);
  CHECK(parse_ok(compile).target_dir == "main.al");
}

TEST_CASE("Parse compile flags") {
  const std::string_view args[] = {"alcy",          "compile", "main.al",
                                   "--release",     "-o",      "main.o",
                                   "--emit=object", "--stdin"};
  const CliConfig config = parse_ok(args);
  CHECK(config.subcommand == Subcommand::Compile);
  CHECK(config.release);
  CHECK(config.output == "main.o");
  CHECK(config.emit == pipeline::EmitMode::Object);
  CHECK(config.stdin_source);
}

TEST_CASE("Parse check file flag") {
  const std::string_view args[] = {"alcy", "check", "--file", "main.al"};
  const CliConfig config = parse_ok(args);
  CHECK(config.subcommand == Subcommand::Check);
  CHECK(config.file == "main.al");
  CHECK(config.target_dir.empty());
}

TEST_CASE("Parse run forwards trailing positionals") {
  const std::string_view args[] = {"alcy", "run", ".", "hello", "world"};
  const CliConfig config = parse_ok(args);
  CHECK(config.subcommand == Subcommand::Run);
  CHECK(config.target_dir == ".");
  CHECK(config.program_args.size() == 2);
  CHECK(config.program_args[0] == "hello");
  CHECK(config.program_args[1] == "world");
}

TEST_CASE("Parse global flags around subcommands") {
  const std::string_view before[] = {"alcy", "--color=never", "build"};
  const CliConfig before_config = parse_ok(before);
  CHECK(before_config.color_mode == term::ColorMode::Never);
  CHECK(before_config.subcommand == Subcommand::Build);

  const std::string_view after[] = {"alcy", "build", "--color=never"};
  const CliConfig after_config = parse_ok(after);
  CHECK(after_config.color_mode == term::ColorMode::Never);
  CHECK(after_config.subcommand == Subcommand::Build);
}

TEST_CASE("Parse time-trace flag") {
  const std::string_view args[] = {"alcy", "-t", "build"};
  const CliConfig config = parse_ok(args);
  CHECK(config.time_trace);
  CHECK(config.subcommand == Subcommand::Build);
}

TEST_CASE("Parse bare invocation has no subcommand") {
  const std::string_view args[] = {"alcy"};
  CHECK(parse(args).is<NoSubcommand>());
}

TEST_CASE("Parse unknown subcommand keeps its name") {
  const std::string_view args[] = {"alcy", "frobnicate"};
  ParseOutcome outcome = parse(args);
  CHECK(outcome.is<UnknownSubcommand>());
  if (outcome.is<UnknownSubcommand>()) {
    CHECK(std::move(outcome).get<UnknownSubcommand>().name == "frobnicate");
  }
}

TEST_CASE("Parse unknown flags") {
  const std::string_view args[] = {"alcy", "build", "--frobnicator"};
  ParseOutcome outcome = parse(args);
  CHECK(outcome.is<ParseFailure>());
  if (outcome.is<ParseFailure>()) {
    CHECK(!std::move(outcome).get<ParseFailure>().errors.empty());
  }
}

TEST_CASE("Parse help and version requests") {
  const std::string_view help[] = {"alcy", "--help"};
  const std::string_view sub_help[] = {"alcy", "build", "--help"};
  const std::string_view version[] = {"alcy", "--version"};
  CHECK(parse(help).is<HelpRequested>());
  CHECK(parse(sub_help).is<HelpRequested>());
  CHECK(parse(version).is<VersionRequested>());
}

TEST_CASE("Parse argv overload") {
  const char* argv[] = {"alcy", "build", "--release", "mydir"};
  arg::Parser parser = build_parser();
  ParseOutcome outcome = parse_args(parser, 4, argv);
  CHECK(outcome.is<CliConfig>());
  if (outcome.is<CliConfig>()) {
    const CliConfig config = outcome.get<CliConfig>();
    CHECK(config.subcommand == Subcommand::Build);
    CHECK(config.release);
    CHECK(config.target_dir == "mydir");
  }
}

}  // namespace cli
