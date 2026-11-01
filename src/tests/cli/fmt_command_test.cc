// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/fmt_command.h"

#include <string>

#include "cli/cli_config.h"
#include "doctest/doctest.h"

namespace cli {
TEST_CASE("A changed line reads as its removal and its replacement") {
  const std::string diff =
      render_diff("main.al", "fn f() {\n  ret 1\n}\n\nfn g() {\n  ret 2\n}\n",
                  "fn f() {\n  ret 1\n}\n\nfn g() {\n  ret 3\n}\n");
  CHECK(diff ==
        "--- main.al\n"
        "+++ main.al\n"
        "@@ -3,5 +3,5 @@\n"
        " }\n"
        " \n"
        " fn g() {\n"
        "-  ret 2\n"
        "+  ret 3\n"
        " }\n");
}
TEST_CASE("Two changes far apart take a hunk each") {
  const std::string diff =
      render_diff("main.al",
                  "fn f() {\n  ret 1\n}\n\nfn g() {\n  ret 2\n}\n\n"
                  "fn h() {\n  ret 3\n}\n\nfn i() {\n  ret 4\n}\n",
                  "fn f() {\n  ret 1\n}\n\nfn g() {\n  ret 20\n}\n\n"
                  "fn h() {\n  ret 3\n}\n\nfn i() {\n  ret 40\n}\n");
  CHECK(diff ==
        "--- main.al\n"
        "+++ main.al\n"
        "@@ -3,7 +3,7 @@\n"
        " }\n"
        " \n"
        " fn g() {\n"
        "-  ret 2\n"
        "+  ret 20\n"
        " }\n"
        " \n"
        " fn h() {\n"
        "@@ -11,5 +11,5 @@\n"
        " }\n"
        " \n"
        " fn i() {\n"
        "-  ret 4\n"
        "+  ret 40\n"
        " }\n");
}
TEST_CASE("A change in the last line ends the hunk there") {
  const std::string diff =
      render_diff("main.al", "fn f() {}\nfn g() {}{{\n", "fn f() {}\n");
  CHECK(diff ==
        "--- main.al\n"
        "+++ main.al\n"
        "@@ -1,2 +1,1 @@\n"
        " fn f() {}\n"
        "-fn g() {}{{\n");
}
TEST_CASE("Layouts that agree produce no diff") {
  CHECK(render_diff("main.al", "fn f() {}\n", "fn f() {}\n").empty());
  CHECK(render_diff("main.al", "", "").empty());
}
TEST_CASE("fmt leaves standard input alone when it names no file") {
  CliConfig config{};
  config.subcommand = Subcommand::Fmt;
  CHECK_FALSE(reads_stdin(config));
}
TEST_CASE("fmt reads standard input when --stdin asks for it") {
  CliConfig config{};
  config.subcommand = Subcommand::Fmt;
  config.fmt_stdin = true;
  CHECK(reads_stdin(config));
}
TEST_CASE("fmt reads the named files instead of standard input") {
  CliConfig config{};
  config.subcommand = Subcommand::Fmt;
  config.fmt_files = {"main.al"};
  CHECK_FALSE(reads_stdin(config));
}
}  // namespace cli
