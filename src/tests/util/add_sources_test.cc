// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <initializer_list>
#include <string_view>
#include <vector>

#include "doctest/doctest.h"
#include "source/source.h"
#include "tests/util/virtual_source.h"

namespace tests {

namespace {

constexpr std::initializer_list<VirtualSource> NO_FILES{};

}  // namespace

TEST_CASE("add_sources names the entry module with an empty name") {
  source::SourceManager sources;
  const SourceSet set = add_sources(
      sources, {{"main.al", "fn main() {}\n"}, {"util.al", "fn help() {}\n"}},
      "main.al");
  CHECK(set.inputs.size() == 2);
  if (set.inputs.size() != 2) {
    return;
  }
  // The analyzer tells the entry module from the rest by its name being
  // empty, not by its position.
  CHECK(set.inputs[0].name.empty());
  CHECK(set.inputs[1].name == "util");
}

TEST_CASE("add_sources drops the extension from a module name") {
  source::SourceManager sources;
  const SourceSet set =
      add_sources(sources, {{"main.al", ""}, {"a/b.al", ""}}, "main.al");
  CHECK(set.inputs.size() == 2);
  if (set.inputs.size() != 2) {
    return;
  }
  // The path is kept whole: the analyzer derives the nesting from it, so
  // "a/b" names a module "b" inside "a" rather than one called "a/b".
  CHECK(set.inputs[1].name == "a/b");
}

TEST_CASE("add_sources keeps a name that has no extension") {
  source::SourceManager sources;
  const SourceSet set =
      add_sources(sources, {{"main.al", ""}, {"weird", ""}}, "main.al");
  CHECK(set.inputs.size() == 2);
  if (set.inputs.size() != 2) {
    return;
  }
  CHECK(set.inputs[1].name == "weird");
}

TEST_CASE("add_sources keeps a name that is only an extension") {
  source::SourceManager sources;
  // ".al" is a name of length equal to the extension, so the extension is
  // not stripped from it: a longer name that merely ends in it is left
  // alone for the same reason.
  const SourceSet set =
      add_sources(sources, {{"main.al", ""}, {".al", ""}}, "main.al");
  CHECK(set.inputs.size() == 2);
  if (set.inputs.size() != 2) {
    return;
  }
  CHECK(set.inputs[1].name == ".al");
}

TEST_CASE("add_sources registers the bytes it was given") {
  source::SourceManager sources;
  const SourceSet set = add_sources(
      sources, {{"main.al", "fn main() {}\n"}, {"util.al", "fn help() {}\n"}},
      "main.al");
  CHECK(set.inputs.size() == 2);
  if (set.inputs.size() != 2) {
    return;
  }
  CHECK(sources.bytes(set.inputs[0].id).value_or("") == "fn main() {}\n");
  CHECK(sources.bytes(set.inputs[1].id).value_or("") == "fn help() {}\n");
  CHECK(sources.file_count() == 2);
  CHECK(!sources.is_mapped(set.inputs[0].id));
}

TEST_CASE("add_sources accepts an empty file list") {
  source::SourceManager sources;
  const SourceSet set = add_sources(sources, NO_FILES, "main.al");
  CHECK(set.inputs.empty());
  CHECK(sources.file_count() == 0);
}

}  // namespace tests
