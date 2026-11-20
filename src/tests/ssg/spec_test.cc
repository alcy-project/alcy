// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <ios>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "doctest/doctest.h"
#include "fpag/io/io_util.h"
#include "fpag/io/temp_dir.h"
#include "ssg/discover.h"
#include "ssg/documents.h"
#include "ssg/markdown.h"
#include "tests/ssg/cases.h"
using ssg::Blocks;
using ssg::BlockType;
using ssg::discover_spec;
using ssg::discover_spec_assets;
using ssg::DocSource;
using ssg::SpecAsset;

namespace {

// A scratch directory the discovery reads: fpag's TempDir wipes and
// recreates it on construction and removes it on destruction, so
// the cases here cannot leak state into one another.
using TempDir = io::TempDir;

void write_file(io::TempDir& dir,
                std::string_view rel,
                std::string_view content) {
  CHECK(dir.write_file(rel, content));
}
}  // namespace

TEST_CASE("a spec file a page links to becomes reachable") {
  const std::set<std::string> published = {"types.md"};
  const Blocks input = {
      ssg_test::paragraph_block("see `types.md` for the tower", 1),
  };
  Blocks blocks = link_sibling_references(input, published);
  CHECK(blocks.size() == 1);
  CHECK(blocks[0].text.find("[`types.md`](types.html)") != std::string::npos);
}

TEST_CASE("a list's items are linked too") {
  const std::set<std::string> published = {"items.md"};
  const Blocks input = {
      ssg_test::list_block({"see `items.md`"}, false, 1),
  };
  Blocks blocks = link_sibling_references(input, published);
  CHECK(blocks.size() == 1);
  CHECK(blocks[0].items[0] == "see [`items.md`](items.html)");
}

TEST_CASE("a name with no page stays text") {
  const std::set<std::string> published = {"types.md"};
  const Blocks input = {
      ssg_test::paragraph_block("write `notes.md` if you like", 1),
  };
  Blocks blocks = link_sibling_references(input, published);
  CHECK(blocks[0].text == "write `notes.md` if you like");
}

TEST_CASE("a fence's text is left alone") {
  const std::set<std::string> published = {"types.md"};
  const Blocks input = {
      ssg_test::code_block("text", "`types.md`", 1),
  };
  Blocks blocks = link_sibling_references(input, published);
  CHECK(blocks[0].text == "`types.md`");
}
TEST_CASE("the reference keeps its editorial order") {
  io::TempDir dir("spec");
  write_file(dir, "deferred.md", "# Deferred\n\nbody\n");
  write_file(dir, "overview.md", "# Overview\n\nbody\n");
  write_file(dir, "zam.md", "# Zed\n\nbody\n");
  auto pages = discover_spec(dir.path());
  CHECK(pages.is_ok());
  const std::vector<DocSource> list = std::move(pages).unwrap();
  CHECK(list.size() == 3);
  CHECK(list[0].source_rel == "overview.md");
  CHECK(list[0].order == 0);
  CHECK(list[1].source_rel == "deferred.md");
  CHECK(list[1].order == 14);
  CHECK(list[2].source_rel == "zam.md");
  CHECK(list[2].order == 15);
}

TEST_CASE("the spec assets are the normative files") {
  io::TempDir dir("assets");
  write_file(dir, "grammar.ebnf", "grammar\n");
  write_file(dir, "notes.txt", "notes\n");
  write_file(dir, "overview.md", "# Overview\n\nbody\n");
  auto assets = discover_spec_assets(dir.path());
  CHECK(assets.is_ok());
  const std::vector<SpecAsset> list = std::move(assets).unwrap();
  CHECK(list.size() == 2);
  CHECK(list[0].path == "reference/grammar.ebnf");
  CHECK(list[1].path == "reference/notes.txt");
}

TEST_CASE("the spec discovery rewrites a sibling reference") {
  io::TempDir dir("spec-link");
  write_file(dir, "types.md", "# Types\n\nsee `modules.md`\n");
  write_file(dir, "modules.md", "# Modules\n\nbody\n");
  auto pages = discover_spec(dir.path());
  CHECK(pages.is_ok());
  const std::vector<DocSource> list = std::move(pages).unwrap();
  for (const DocSource& source : list) {
    if (source.source_rel == "types.md") {
      CHECK_FALSE(source.blocks.empty());
      bool found = false;
      if (source.blocks[1].type == BlockType::Paragraph) {
        found = source.blocks[1].text.find("[`modules.md`](modules.html)") !=
                std::string::npos;
      }
      CHECK(found);
    }
  }
}
