// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/cli_config.h"

#include <cstddef>
#include <initializer_list>
#include <string_view>

#include "doctest/doctest.h"
#include "pipeline/link_options.h"
#include "pkg/toolchain.h"

namespace cli {

namespace {

constexpr std::string_view FILE_DRIVER = "driver-from-file";
constexpr std::string_view FILE_ARGS[] = {"-from-file"};

// The goal package's file, written out rather than parsed: what is under
// test here is how an invocation resolves against it.
pkg::Toolchain file() {
  return pkg::Toolchain{.linker = FILE_DRIVER, .link_args = FILE_ARGS};
}

// Arguments the command named, as the parser collects them.
CliConfig with_args(std::initializer_list<std::string_view> arguments) {
  CliConfig config;
  config.link_args.assign(arguments.begin(), arguments.end());
  return config;
}

}  // namespace

TEST_CASE("An invocation naming nothing links the way the file says") {
  const pipeline::LinkOptions link = resolve_link_options(CliConfig{}, file());
  CHECK(link.driver == FILE_DRIVER);
  CHECK(link.args.size() == 1);
  if (link.args.size() == 1) {
    CHECK(link.args[0] == "-from-file");
  }
}

TEST_CASE("A named driver replaces the file's driver") {
  CliConfig config;
  config.linker = "driver-from-flag";
  const pipeline::LinkOptions link = resolve_link_options(config, file());
  CHECK(link.driver == "driver-from-flag");
  // The arguments were not named, so they still come from the file.
  CHECK(link.args.size() == 1);
  if (link.args.size() == 1) {
    CHECK(link.args[0] == "-from-file");
  }
}

TEST_CASE("Named arguments replace the file's list rather than join it") {
  const CliConfig config = with_args({"-from-flag", "-also-from-flag"});
  const pipeline::LinkOptions link = resolve_link_options(config, file());
  CHECK(link.args.size() == 2);
  if (link.args.size() == 2) {
    CHECK(link.args[0] == "-from-flag");
    CHECK(link.args[1] == "-also-from-flag");
  }
  CHECK(link.driver == FILE_DRIVER);
}

TEST_CASE("A command that reads no file links from the command alone") {
  CliConfig config;
  config.linker = "driver-from-flag";
  config.link_args = {"-from-flag"};
  const pipeline::LinkOptions link =
      resolve_link_options(config, pkg::Toolchain{});
  CHECK(link.driver == "driver-from-flag");
  CHECK(link.args.size() == 1);
  if (link.args.size() == 1) {
    CHECK(link.args[0] == "-from-flag");
  }
}

}  // namespace cli
