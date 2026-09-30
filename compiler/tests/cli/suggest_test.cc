// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/suggest.h"

#include <optional>
#include <string>
#include <string_view>

#include "cli/parse_args.h"
#include "doctest/doctest.h"
#include "fpag/arg/parser.h"

namespace cli {

TEST_CASE("Affix matching covers both directions") {
  CHECK(is_affix_match("buil", "build"));
  CHECK(is_affix_match("build", "buil"));
  CHECK(is_affix_match("trace", "time-trace"));
  CHECK(is_affix_match("time-trace", "trace"));
  CHECK(is_affix_match("output-file", "output"));
  CHECK(!is_affix_match("frobnicate", "build"));
  CHECK(!is_affix_match("run", "check"));
}

TEST_CASE("Edit distance counts substitutions and transpositions") {
  CHECK(edit_distance("build", "build") == 0);
  CHECK(edit_distance("", "build") == 5);
  CHECK(edit_distance("buid", "build") == 1);
  CHECK(edit_distance("comiple", "compile") == 1);
  CHECK(edit_distance("chek", "check") == 1);
  CHECK(edit_distance("frobnicate", "build") > 3);
}

TEST_CASE("Best match prefers affix, then distance, then order") {
  const std::string_view commands[] = {"build", "compile", "run",
                                       "new",   "init",    "check"};
  CHECK(best_match("buid", commands) == std::string_view("build"));
  CHECK(best_match("comiple", commands) == std::string_view("compile"));
  CHECK(best_match("chek", commands) == std::string_view("check"));
  CHECK(!best_match("frobnicate", commands).has_value());
  CHECK(!best_match("", commands).has_value());

  const std::string_view flags[] = {"release", "output", "emit", "linker",
                                    "link-args"};
  CHECK(best_match("outpu", flags) == std::string_view("output"));
  CHECK(best_match("args", flags) == std::string_view("link-args"));
  const std::string_view traced[] = {"time-trace", "trace-check"};
  CHECK(best_match("trace", traced) == std::string_view("time-trace"));

  // A tie breaks lexically, so the answer never depends on order.
  const std::string_view tied[] = {"abd", "abc"};
  CHECK(best_match("abx", tied) == std::string_view("abc"));
}

TEST_CASE("Subcommand suggestion names the closest command") {
  arg::Parser parser = build_parser();
  CHECK(suggest_subcommand(parser.root_command(), "buid") ==
        std::optional<std::string>("build"));
  CHECK(suggest_subcommand(parser.root_command(), "chek") ==
        std::optional<std::string>("check"));
  CHECK(!suggest_subcommand(parser.root_command(), "frobnicate").has_value());
}

TEST_CASE("Flag suggestion stays in the selected scope") {
  arg::Parser parser = build_parser();
  const arg::Command& root = parser.root_command();

  const std::string_view build_typo[] = {"alcy", "build", "--outpu"};
  CHECK(suggest_flag(root, build_typo, "outpu") ==
        std::optional<std::string>("output"));

  const std::string_view new_typo[] = {"alcy", "new", "--vsc"};
  CHECK(suggest_flag(root, new_typo, "vsc") ==
        std::optional<std::string>("vcs"));

  // `vcs` belongs to `new` and `init`, so a `build` invocation cannot
  // mean it no matter how close the spelling is.
  const std::string_view wrong_scope[] = {"alcy", "build", "--vsc"};
  CHECK(!suggest_flag(root, wrong_scope, "vsc").has_value());

  const std::string_view far[] = {"alcy", "build", "--frobnicator"};
  CHECK(!suggest_flag(root, far, "frobnicator").has_value());

  const std::string_view help_typo[] = {"alcy", "--halp"};
  CHECK(suggest_flag(root, help_typo, "halp") ==
        std::optional<std::string>("help"));
}

TEST_CASE("Flag scope skips values the parser would consume") {
  arg::Parser parser = build_parser();
  const arg::Command& root = parser.root_command();

  // `--color` takes `check` as its value, so no subcommand is selected
  // and the root flags are the only candidates.
  const std::string_view consumed[] = {"alcy", "--color", "check", "--fil"};
  CHECK(!suggest_flag(root, consumed, "fil").has_value());

  const std::string_view short_consumed[] = {"alcy", "-o", "check", "--fil"};
  CHECK(!suggest_flag(root, short_consumed, "fil").has_value());
}

}  // namespace cli
