// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <string>
#include <utility>
#include <vector>

#include "doctest/doctest.h"
#include "ssg/documents.h"
#include "ssg/markdown.h"
#include "ssg/render.h"
#include "tests/ssg/cases.h"
using ssg::Block;
using ssg::DocPage;
using ssg::DocSource;

namespace {

const std::vector<DocPage> ONE_PAGE = {
    ssg_test::test_page("Title", "guide/x.html", "en"),
};

std::string render(std::string_view language, std::vector<Block> blocks) {
  return ssg_test::render_article(ONE_PAGE, ssg::GUIDE_TREE, language,
                                  std::move(blocks));
}

}  // namespace

TEST_CASE("sections gain anchors and an outline beside the article") {
  const std::string html = render(
      "en",
      {ssg_test::prose_block(), ssg_test::heading_block(2, "Values", 3),
       ssg_test::prose_block(), ssg_test::heading_block(2, "Functions", 5),
       ssg_test::prose_block()});
  CHECK(html.find("<h2 id=\"values\">Values</h2>") != std::string::npos);
  CHECK(html.find("<h2 id=\"functions\">Functions</h2>") != std::string::npos);
  CHECK(html.find("<div class=\"guide-aside\">") != std::string::npos);
  CHECK(html.find("<details class=\"guide-outline\">") != std::string::npos);
  CHECK(html.find("<summary>On this page</summary>") != std::string::npos);
  CHECK(html.find("<li><a href=\"#functions\">Functions</a></li>") !=
        std::string::npos);
}

TEST_CASE("only a runnable block loads the compiler half of the guide") {
  const std::string examples = render("en", {ssg_test::shell_block()});
  CHECK(examples.find("guide-copy.js") != std::string::npos);
  CHECK(examples.find("shared/guide.js\"") == std::string::npos);
  const std::string runnable = render("en", {ssg_test::alcy_block()});
  CHECK(runnable.find("shared/guide-copy.js") != std::string::npos);
  CHECK(runnable.find("shared/guide.js") != std::string::npos);
}

TEST_CASE("the outline says how much each section asks of the reader") {
  const std::string html = render(
      "en", {ssg_test::heading_block(1, "Title", 1),
             ssg_test::heading_block(2, "Values", 3), ssg_test::alcy_block(),
             ssg_test::alcy_block(), ssg_test::heading_block(2, "Functions", 7),
             ssg_test::alcy_block(), ssg_test::heading_block(2, "Notes", 11),
             ssg_test::prose_block()});
  CHECK(html.find("Values <span class=\"outline-tally\">2 programs</span>") !=
        std::string::npos);
  CHECK(html.find("Functions <span class=\"outline-tally\">1 program</span>") !=
        std::string::npos);
  CHECK(html.find("Notes</a>") != std::string::npos);
}

TEST_CASE("the outline counts only the fences that run") {
  const std::string html = render(
      "en",
      {ssg_test::heading_block(1, "Title", 1),
       ssg_test::heading_block(2, "Values", 3), ssg_test::shell_block(),
       ssg_test::heading_block(3, "Detail", 5), ssg_test::alcy_block(),
       ssg_test::heading_block(2, "Functions", 9), ssg_test::prose_block()});
  CHECK(html.find("Values <span class=\"outline-tally\">1 program</span>") !=
        std::string::npos);
  CHECK(html.find("Functions</a>") != std::string::npos);
}

TEST_CASE("a page without sections carries no outline") {
  const std::string html = render("en", {ssg_test::prose_block()});
  CHECK(html.find("guide-outline") == std::string::npos);
}

TEST_CASE("the footer carries the build stamp linked to its source") {
  const std::string html = render("en", {ssg_test::prose_block()});
  CHECK(html.find("<span>alcy build <a "
                  "href=\"https://example.test/commit/TESTSTAMP\" "
                  "rel=\"noopener\">TESTSTAMP</a></span>") !=
        std::string::npos);
}

TEST_CASE("the theme bootstrap rides inline in the head") {
  const std::string html = render("en", {ssg_test::prose_block()});
  CHECK(html.find("<script>INLINE_BOOT</script>") != std::string::npos);
  CHECK(html.find("boot.js") == std::string::npos);
}

TEST_CASE("a code block gains a copy button hidden until a script reveals it") {
  const std::string html = render("en", {ssg_test::shell_block()});
  CHECK(html.find("<div class=\"code-block\">") != std::string::npos);
  CHECK(html.find("<div class=\"code-actions\" hidden>") != std::string::npos);
  CHECK(html.find("data-action=\"copy\">Copy") != std::string::npos);
}

TEST_CASE("the copy label is stamped in the page language") {
  const std::string html = render("ja", {ssg_test::shell_block()});
  CHECK(html.find("\xe3\x82\xb3\xe3\x83\x94\xe3\x83\xbc") != std::string::npos);
}

TEST_CASE("a non-Latin heading keeps its characters as the anchor") {
  // The heading is the Japanese word for "introduction".
  const std::string heading =
      "\xe3\x81\xaf\xe3\x81\x98\xe3\x82\x81\xe3\x81\xab";
  const std::string html =
      render("ja", {ssg_test::heading_block(2, heading, 1)});
  std::string expected = "<h2 id=\"";
  expected += heading;
  expected += "\">";
  expected += heading;
  expected += "</h2>";
  CHECK(html.find(expected) != std::string::npos);
}

TEST_CASE("the article carries the page's language, not a fixed English") {
  const std::string html = render("ja", {ssg_test::prose_block()});
  CHECK(html.find("lang=\"ja\"") != std::string::npos);
}

TEST_CASE("the guide assets load only when a page has code") {
  const std::string with_code = render("en", {ssg_test::alcy_block()});
  CHECK(with_code.find("shared/guide.css") != std::string::npos);
  CHECK(with_code.find("shared/guide.js") != std::string::npos);
  const std::string without = render("en", {ssg_test::prose_block()});
  CHECK(without.find("shared/guide.") == std::string::npos);
}

TEST_CASE("an alcy block gains edit, run, and an output panel") {
  const std::string html = render("en", {ssg_test::alcy_block()});
  CHECK(html.find("<div class=\"code-block\" data-language=\"alcy\">") !=
        std::string::npos);
  CHECK(html.find("data-action=\"edit\" aria-pressed=\"false\">Edit") !=
        std::string::npos);
  CHECK(html.find("data-action=\"reset\" hidden>Reset") != std::string::npos);
  CHECK(html.find("data-action=\"run\">Run") != std::string::npos);
  CHECK(html.find("data-action=\"open\"") != std::string::npos);
  CHECK(html.find("<div class=\"code-output\" hidden></div>") !=
        std::string::npos);
}

TEST_CASE("a non-alcy block stays copy-only") {
  const std::string html = render("en", {ssg_test::shell_block()});
  CHECK(html.find("data-action=\"edit\"") == std::string::npos);
  CHECK(html.find("data-action=\"run\"") == std::string::npos);
  CHECK(html.find("code-output") == std::string::npos);
  CHECK(html.find("data-language") == std::string::npos);
}

TEST_CASE("the alcy labels are stamped in the page language") {
  const std::string html = render("ja", {ssg_test::alcy_block()});
  CHECK(html.find("\xe7\xb7\xa8\xe9\x9b\x86") != std::string::npos);
  CHECK(html.find("\xe5\xae\x9f\xe8\xa1\x8c") != std::string::npos);
}

TEST_CASE("a page names its canonical and alternate addresses") {
  const std::string html = render("en", {ssg_test::prose_block()});
  CHECK(html.find(
            "<link rel=\"canonical\" "
            "href=\"https://alcy-project.github.io/alcy/guide/x.html\" />") !=
        std::string::npos);
  CHECK(html.find("https://alcy-project.github.io/alcy/ja/guide/x.html") !=
        std::string::npos);
}

TEST_CASE("a page claims no editor key and loads its own tree") {
  ;
  const std::string html = render("ja", {ssg_test::prose_block()});
  CHECK(html.find("shared/i18n.ja.js") != std::string::npos);
}

TEST_CASE("the guide last page continues into the reference") {
  const std::vector<DocPage> pages = {
      ssg_test::test_page("First", "guide/introduction.html", "en"),
      ssg_test::test_page("Last", "guide/z.html", "en"),
  };
  const std::string html = ssg_test::render_article(
      pages, ssg::GUIDE_TREE, "en", {ssg_test::prose_block()});
  CHECK(html.find("pager-onward") != std::string::npos);
  CHECK(html.find("../reference/index.html") != std::string::npos);
}

TEST_CASE("the reference does not send a reader back into the guide") {
  const std::vector<DocPage> pages = {
      ssg_test::test_page("X", "reference/x.html", "en"),
  };
  const std::string html = ssg_test::render_article(
      pages, ssg::REFERENCE_TREE, "en", {ssg_test::prose_block()});
  CHECK(html.find("pager-onward") == std::string::npos);
}

TEST_CASE(
    "a translation replaces its page and missing ones fall back to the "
    "default") {
  const auto source = [](std::string_view slug, std::string_view language) {
    DocSource doc;
    doc.source_rel = std::string(language) + "/" + std::string(slug) + ".md";
    doc.language = std::string(language);
    doc.path = "guide/" + std::string(slug) + ".html";
    doc.title = std::string(slug);
    return doc;
  };
  const std::vector<DocSource> sources = {
      source("intro", "en"), source("tour", "en"), source("tour", "ja")};

  const std::vector<DocPage> pages =
      ssg::pages_for_language(ssg_test::test_catalog(), sources, "ja");
  CHECK(pages.size() == 2);
  if (pages.size() != 2) {
    return;
  }
  CHECK(pages[0].source_rel == "en/intro.md");
  CHECK(pages[0].route == "ja/guide/intro.html");
  CHECK(pages[1].source_rel == "ja/tour.md");
  CHECK(pages[1].language == "ja");
  CHECK(pages[1].route == "ja/guide/tour.html");
}
