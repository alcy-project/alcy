// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// What a page owes a reader who is not running its scripts, and a
// reader who is not looking at it at all. The generator stamps the
// article, the labels, and the language, so these checks hold the
// properties rather than a markup shape: the words are in the
// document, every control that ships hidden hides a control and not
// content, the landmarks are named, and the language the document
// declares is the language its strings are in.

#include <regex>
#include <set>
#include <string>
#include <vector>

#include "doctest/doctest.h"
#include "ssg/documents.h"
#include "ssg/render.h"
#include "tests/ssg/cases.h"

namespace {

using ssg::DocPage;
using ssg_test::alcy_block;
using ssg_test::prose_block;
using ssg_test::shell_block;
using ssg_test::test_page;

// The src of every script that loads with the page, in order.
std::vector<std::string> scripts(const std::string& html) {
  std::vector<std::string> srcs;
  const std::regex src_re("<script[^>]*src=\"([^\"]+)\"");
  for (std::sregex_iterator it(html.begin(), html.end(), src_re), end;
       it != end; ++it) {
    srcs.push_back((*it)[1].str());
  }
  return srcs;
}

// The aria-label of every <nav>, in document order.
std::vector<std::string> nav_labels(const std::string& html) {
  std::vector<std::string> labels;
  const std::regex nav_re("<nav[^>]*aria-label=\"([^\"]+)\"");
  for (std::sregex_iterator it(html.begin(), html.end(), nav_re), end;
       it != end; ++it) {
    labels.push_back((*it)[1].str());
  }
  return labels;
}

// The open tags that carry a `hidden` attribute, as full tags. An
// attribute value with the word inside it does not match: the check
// is for the attribute as a token.
std::vector<std::string> hidden_tags(const std::string& html) {
  std::vector<std::string> tags;
  const std::regex tag_re("<([a-z]+)([^>]*)>");
  for (std::sregex_iterator it(html.begin(), html.end(), tag_re), end;
       it != end; ++it) {
    const std::string attrs = (*it)[2].str();
    if (attrs.find(" hidden") == std::string::npos) {
      continue;
    }
    tags.push_back((*it)[0].str());
  }
  return tags;
}

}  // namespace

TEST_CASE("the article's prose is in the document, not assembled by a script") {
  const std::string html =
      ssg_test::render_article({}, ssg::GUIDE_TREE, "en",
                               {ssg_test::heading_block(1, "The tower", 1),
                                ssg_test::paragraph_block("hello reader", 2)});
  CHECK(html.find("hello reader") != std::string::npos);
  CHECK(html.find("The tower") != std::string::npos);
}

TEST_CASE("a code block's text survives with its buttons hidden") {
  const std::string html =
      ssg_test::render_article({}, ssg::GUIDE_TREE, "en", {alcy_block()});
  CHECK(html.find("fn main() {}") != std::string::npos);
  CHECK(html.find("<div class=\"code-actions\" hidden>") != std::string::npos);
}

TEST_CASE("the output panel is hidden until a program runs") {
  const std::string html =
      ssg_test::render_article({}, ssg::GUIDE_TREE, "en", {alcy_block()});
  CHECK(html.find("<div class=\"code-output\" hidden></div>") !=
        std::string::npos);
}

TEST_CASE("nothing the reader sees depends on JavaScript being on") {
  const std::string html = ssg_test::render_article(
      {}, ssg::GUIDE_TREE, "en", {alcy_block(), shell_block()});
  const std::vector<std::string> tags = hidden_tags(html);
  CHECK_FALSE(tags.empty());
  for (const std::string& tag : tags) {
    const bool is_control = tag.find("code-actions") != std::string::npos ||
                            tag.find("code-output") != std::string::npos;
    const bool is_button =
        tag.starts_with("<button") || tag.starts_with("<svg");
    const bool is_safe = is_control || is_button;
    CHECK_MESSAGE(is_safe, tag, " hides content a reader cannot reach");
  }
  const usize open_main = html.find("<main");
  const usize close_main = html.find("</main");
  CHECK(open_main < close_main);
  CHECK(html.find(
            "<main class=\"guide-article\" id=\"content\" tabindex=\"-1\">") ==
        std::string::npos);
}

TEST_CASE("a page without code loads no script beyond the shell") {
  const std::vector<std::string> srcs = scripts(
      ssg_test::render_article({}, ssg::GUIDE_TREE, "en", {prose_block()}));
  CHECK(srcs.size() == 1);
  CHECK(srcs[0].ends_with("shared/shell.js"));
}

TEST_CASE("the contents page is a list of links, not a script") {
  const std::vector<DocPage> pages = {test_page("Title", "guide/x.html")};
  const ssg::Stamp stamp = {.text = "TESTSTAMP", .url = ""};
  const std::string html =
      ssg::render_tree_index(pages, ssg_test::test_catalog(), "en",
                             "INLINE_BOOT", stamp, ssg::GUIDE_TREE);
  CHECK(html.find("<li><a href=\"x.html\">Title</a></li>") !=
        std::string::npos);
}

TEST_CASE("the 404 page keeps its words and its way home") {
  const std::string html = ssg::render_not_found(
      "INLINE_BOOT", {.text = "TESTSTAMP", .url = ""},
      [](std::string_view code) { return std::string(code); });
  CHECK(html.find("Page not found") != std::string::npos);
  CHECK(html.find("href=\"guide/\"") != std::string::npos);
}

TEST_CASE("the editor is fetched on the first Edit, not with the page") {
  const std::vector<std::string> srcs = scripts(
      ssg_test::render_article({}, ssg::GUIDE_TREE, "en", {alcy_block()}));
  bool names_guide = false;
  for (const std::string& src : srcs) {
    CHECK(src.find("guide-edit.js") == std::string::npos);
    if (src.ends_with("shared/guide.js")) {
      names_guide = true;
    }
  }
  CHECK(names_guide);
}

TEST_CASE("a reference page without a program loads no compiler") {
  const std::vector<std::string> srcs = scripts(
      ssg_test::render_article({}, ssg::REFERENCE_TREE, "en", {prose_block()}));
  for (const std::string& src : srcs) {
    CHECK(src.find("guide.js") == std::string::npos);
  }
}

TEST_CASE("each document declares the language of its own strings") {
  CHECK(ssg_test::render_article({}, ssg::GUIDE_TREE, "en", {prose_block()})
            .find("<html lang=\"en\">") != std::string::npos);
  CHECK(ssg_test::render_article({}, ssg::GUIDE_TREE, "ja", {prose_block()})
            .find("<html lang=\"ja\">") != std::string::npos);
}

TEST_CASE("the prose declares its own language inside a translated tree") {
  const std::string ja =
      ssg_test::render_article({}, ssg::GUIDE_TREE, "ja", {prose_block()});
  CHECK(ja.find("<html lang=\"ja\">") != std::string::npos);
  CHECK(ja.find("lang=\"en\"") != std::string::npos);
}

TEST_CASE("the page's regions are named landmarks") {
  const std::string html =
      ssg_test::render_article({}, ssg::GUIDE_TREE, "en", {prose_block()});
  CHECK(html.find("<header") != std::string::npos);
  CHECK(html.find("<main") != std::string::npos);
  CHECK(html.find("<footer") != std::string::npos);
  const std::vector<std::string> labels = nav_labels(html);
  CHECK_FALSE(labels.empty());
}

TEST_CASE("no two landmarks answer to the same name") {
  const std::string html =
      ssg_test::render_article({}, ssg::GUIDE_TREE, "en", {prose_block()});
  const std::vector<std::string> labels = nav_labels(html);
  CHECK(labels.size() >= 3);
  std::set<std::string> distinct;
  for (const std::string& label : labels) {
    CHECK_FALSE(label.empty());
    distinct.insert(label);
  }
  CHECK(distinct.size() == labels.size());
}

TEST_CASE("the current page is marked, not just coloured") {
  const std::string html =
      ssg_test::render_article({}, ssg::GUIDE_TREE, "en", {prose_block()});
  CHECK(html.find("aria-current=\"page\"") != std::string::npos);
}

TEST_CASE("the 404 is marked as a dead end and still reads in order") {
  const std::string html = ssg::render_not_found(
      "INLINE_BOOT", {.text = "TESTSTAMP", .url = ""},
      [](std::string_view code) { return std::string(code); });
  CHECK(html.find("name=\"robots\" content=\"noindex\"") != std::string::npos);
  CHECK(html.find("<html lang=\"en\">") != std::string::npos);
  CHECK(html.find("<main") < html.find("data-action=\"copy\""));
}
