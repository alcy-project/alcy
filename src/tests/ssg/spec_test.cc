// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <ios>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "doctest/doctest.h"
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

std::filesystem::path fresh_temp_dir(const char* label) {
  const std::filesystem::path base = std::filesystem::temp_directory_path();
  const std::string name = std::string("alcy-ssg-") + label;
  const std::filesystem::path dir = base / name;
  std::error_code error;
  std::filesystem::remove_all(dir, error);
  std::filesystem::create_directories(dir, error);
  return dir;
}

void write_file(const std::filesystem::path& path, std::string_view content) {
  std::ofstream output(path, std::ios::binary);
  output << content;
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
  const std::filesystem::path dir = fresh_temp_dir("spec");
  write_file(dir / "deferred.md", "# Deferred\n\nbody\n");
  write_file(dir / "overview.md", "# Overview\n\nbody\n");
  write_file(dir / "zam.md", "# Zed\n\nbody\n");
  auto pages = discover_spec(dir);
  CHECK(pages.is_ok());
  const std::vector<DocSource> list = std::move(pages).unwrap();
  CHECK(list.size() == 3);
  CHECK(list[0].source_rel == "overview.md");
  CHECK(list[0].order == 0);
  CHECK(list[1].source_rel == "deferred.md");
  CHECK(list[1].order == 14);
  CHECK(list[2].source_rel == "zam.md");
  CHECK(list[2].order == 15);
  std::error_code error;
  std::filesystem::remove_all(dir, error);
}

TEST_CASE("the spec assets are the normative files") {
  const std::filesystem::path dir = fresh_temp_dir("assets");
  write_file(dir / "grammar.ebnf", "grammar\n");
  write_file(dir / "notes.txt", "notes\n");
  write_file(dir / "overview.md", "# Overview\n\nbody\n");
  auto assets = discover_spec_assets(dir);
  CHECK(assets.is_ok());
  const std::vector<SpecAsset> list = std::move(assets).unwrap();
  CHECK(list.size() == 2);
  CHECK(list[0].path == "reference/grammar.ebnf");
  CHECK(list[1].path == "reference/notes.txt");
  std::error_code error;
  std::filesystem::remove_all(dir, error);
}

TEST_CASE("the spec discovery rewrites a sibling reference") {
  const std::filesystem::path dir = fresh_temp_dir("spec-link");
  write_file(dir / "types.md", "# Types\n\nsee `modules.md`\n");
  write_file(dir / "modules.md", "# Modules\n\nbody\n");
  auto pages = discover_spec(dir);
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
  std::error_code error;
  std::filesystem::remove_all(dir, error);
}
