// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "ssg/markdown.h"

#include <string>
#include <utility>
#include <vector>

#include "doctest/doctest.h"
#include "ssg/documents.h"
#include "ssg/render.h"
#include "tests/ssg/cases.h"
using ssg::Block;
using ssg::BlockType;
using ssg::CellAlign;
using ssg::DocPage;
using ssg::parse_markdown;
using ssg::render_inline;

TEST_CASE("paragraphs join, wrap, and split on blank lines") {
  auto blocks = parse_markdown("one\ntwo\n\nthree\n").unwrap();
  CHECK(blocks.size() == 2);
  CHECK(blocks[0].type == BlockType::Paragraph);
  CHECK(blocks[0].text == "one two");
  CHECK(blocks[0].line == 1);
  CHECK(blocks[1].text == "three");
  CHECK(blocks[1].line == 4);
}

TEST_CASE("headings keep their level and line") {
  auto blocks = parse_markdown("# Title\n\n### Deep").unwrap();
  CHECK(blocks.size() == 2);
  CHECK(blocks[0].level == 1);
  CHECK(blocks[0].text == "Title");
  CHECK(blocks[1].level == 3);
  CHECK(blocks[1].text == "Deep");
  CHECK(blocks[1].line == 3);
}

TEST_CASE("code fences carry their language and text") {
  auto blocks =
      parse_markdown("before\n\n```alcy\nfn main() {}\n```\nafter\n").unwrap();
  CHECK(blocks.size() == 3);
  CHECK(blocks[1].type == BlockType::Code);
  CHECK(blocks[1].language == "alcy");
  CHECK(blocks[1].text == "fn main() {}");
  CHECK(blocks[1].line == 3);
}

TEST_CASE("an unclosed fence is an error") {
  auto parsed = parse_markdown("```alcy\nfn main() {}\n");
  CHECK(parsed.is_err());
}

TEST_CASE("lists of both kinds, with indented continuations") {
  auto blocks =
      parse_markdown("- one\n- two\n  continued\n\n1. first\n2. second\n")
          .unwrap();
  CHECK(blocks.size() == 2);
  CHECK(blocks[0].type == BlockType::List);
  CHECK(!blocks[0].ordered);
  CHECK(blocks[0].items == std::vector<std::string>{"one", "two continued"});
  CHECK(blocks[0].line == 1);
  CHECK(blocks[1].ordered);
  CHECK(blocks[1].items == std::vector<std::string>{"first", "second"});
}

TEST_CASE("a blockquote joins its lines") {
  auto blocks = parse_markdown("> one\n> two\n").unwrap();
  CHECK(blocks.size() == 1);
  CHECK(blocks[0].type == BlockType::Blockquote);
  CHECK(blocks[0].text == "one two");
  CHECK(blocks[0].line == 1);
}

TEST_CASE("inline markup renders and text is escaped") {
  CHECK(render_inline("a `x < y` and **b** and *c* and <script>") ==
        "a <code>x &lt; y</code> and <strong>b</strong> and <em>c</em> and "
        "&lt;script&gt;");
}

TEST_CASE("backslash escapes a marker") {
  CHECK(render_inline("\\*not emphasis\\*") == "*not emphasis*");
}

TEST_CASE("links are resolved and attributes are escaped") {
  auto resolve = [](std::string_view url, void*) {
    return std::string(url) + "?x=\"y\"";
  };
  CHECK(render_inline("[a & b](target.md)", resolve, nullptr) ==
        "<a href=\"target.md?x=&quot;y&quot;\">a &amp; b</a>");
}

TEST_CASE("a link inside a label stays text") {
  CHECK(render_inline("[outer [inner](x.md)](y.md)") ==
        "<a href=\"y.md\">outer [inner](x.md)</a>");
}
TEST_CASE("a table parses into header and body rows") {
  auto blocks = parse_markdown(
                    "| key | values |\n"
                    "| --- | --- |\n"
                    "| `a` | 1 |\n"
                    "| b | 2 |\n")
                    .unwrap();
  CHECK(blocks.size() == 1);
  const Block& table = blocks.front();
  CHECK(table.type == BlockType::Table);
  CHECK(table.rows.size() == 3);
  CHECK(table.rows[0] == std::vector<std::string>{"key", "values"});
  CHECK(table.rows[1] == std::vector<std::string>{"`a`", "1"});
  CHECK(table.rows[2] == std::vector<std::string>{"b", "2"});
}

TEST_CASE("separator colons set the cell alignment") {
  auto blocks = parse_markdown(
                    "| a | b | c |\n"
                    "| :-- | :-: | --: |\n"
                    "| 1 | 2 | 3 |\n")
                    .unwrap();
  CHECK(blocks.size() == 1);
  const Block& table = blocks.front();
  CHECK(table.aligns.size() == 3);
  CHECK(table.aligns[0] == CellAlign::Left);
  CHECK(table.aligns[1] == CellAlign::Center);
  CHECK(table.aligns[2] == CellAlign::Right);
}

TEST_CASE("a pipe inside a code span does not split the cell") {
  auto blocks = parse_markdown(
                    "| a | b |\n"
                    "| --- | --- |\n"
                    "| `x | y` | 1 |\n")
                    .unwrap();
  CHECK(blocks.size() == 1);
  CHECK(blocks.front().rows[1][0] == "`x | y`");
  CHECK(blocks.front().rows[1][1] == "1");
}

TEST_CASE("a backslash-escaped pipe stays cell text") {
  auto blocks = parse_markdown(
                    "| a | b |\n"
                    "| --- | --- |\n"
                    "| x \\| y | 1 |\n")
                    .unwrap();
  CHECK(blocks.size() == 1);
  CHECK(blocks.front().rows[1][0] == "x \\| y");
  CHECK(blocks.front().rows[1][1] == "1");
}

TEST_CASE("a ragged row fails the build with the row line") {
  auto parsed = parse_markdown(
      "| a | b |\n"
      "| --- | --- |\n"
      "| 1 |\n");
  CHECK(parsed.is_err());
  CHECK(std::move(parsed).unwrap_err().find('3') != std::string::npos);
}

TEST_CASE("an escaped inline pipe renders as a literal pipe") {
  CHECK(render_inline("x \\| y") == "x | y");
}

TEST_CASE("tables render as a real table with scope on the header cells") {
  const std::vector<Block> blocks = {
      ssg_test::table_block({{"key", "values"},
                             {"`target.os`", "linux"},
                             {"`target.arch`", "x86_64"}},
                            {CellAlign::None, CellAlign::None}, 1),
  };
  const std::vector<DocPage> pages = {
      ssg_test::test_page("Title", "reference/x.html", "en")};
  const std::string html =
      ssg_test::render_article(pages, ssg::REFERENCE_TREE, "en", blocks);
  CHECK(html.find("<table>") != std::string::npos);
  CHECK(html.find("<th scope=\"col\">key</th>") != std::string::npos);
  CHECK(html.find("<td><code>target.os</code></td>") != std::string::npos);
}
