// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "ssg/links.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "doctest/doctest.h"
#include "ssg/documents.h"
using ssg::DocPage;
using ssg::LinkAudit;
using ssg::LinkContext;

namespace {

DocPage make_page(std::string_view source_rel,
                  std::string_view path,
                  std::string_view language = "en") {
  DocPage page;
  page.source_rel = std::string(source_rel);
  page.path = std::string(path);
  page.order = 0;
  page.title = std::string(source_rel);
  page.language = std::string(language);
  page.route =
      (language == "en" ? std::string() : std::string(language) + "/") +
      std::string(path);
  return page;
}

const std::vector<DocPage> PAGES = {
    make_page("01-introduction.md", "guide/introduction.html"),
    make_page("02-getting-started.md", "guide/getting-started.html"),
    make_page("sub/03-more.md", "guide/sub/more.html"),
    make_page("01-introduction.md", "guide/introduction.html", "ja"),
    make_page("02-getting-started.md", "guide/getting-started.html", "ja"),
    make_page("sub/03-more.md", "guide/sub/more.html", "ja"),
};

LinkAudit make_checker(const std::unordered_set<std::string>& site = {},
                       const std::unordered_set<std::string>& repo = {}) {
  LinkContext context;
  context.pages = &PAGES;
  context.site_exists = [site](std::string_view route) {
    return route.empty() || site.find(std::string(route)) != site.end();
  };
  context.repo_exists = [repo](std::string_view path) {
    return repo.find(std::string(path)) != repo.end();
  };
  return LinkAudit(std::move(context));
}

}  // namespace

TEST_CASE("a page link becomes the built html in the same tree") {
  LinkAudit links = make_checker();
  CHECK(links.resolve("02-getting-started.md", PAGES[0]) ==
        "getting-started.html");
  CHECK(links.resolve("sub/03-more.md", PAGES[0]) == "sub/more.html");
  CHECK(links.resolve("../01-introduction.md#values", PAGES[2]) ==
        "../introduction.html#values");
  CHECK(links.resolve("02-getting-started.md", PAGES[3]) ==
        "getting-started.html");
  CHECK(links.errors().empty());
}

TEST_CASE("a repository markdown link becomes a GitHub URL") {
  LinkAudit links = make_checker({}, {"docs/spec/01-lexical.md"});
  CHECK(
      links.resolve("../spec/01-lexical.md", PAGES[0]) ==
      "https://github.com/alcy-project/alcy/blob/main/docs/spec/01-lexical.md");
  CHECK(links.resolve("../spec/01-lexical.md#decision", PAGES[1]) ==
        "https://github.com/alcy-project/alcy/blob/main/docs/spec/"
        "01-lexical.md#decision");
  CHECK(links.errors().empty());
}

TEST_CASE("a missing repository file is an error") {
  LinkAudit links = make_checker();
  CHECK(links.resolve("../spec/99-nope.md", PAGES[0]) == "../spec/99-nope.md");
  CHECK(links.errors().size() == 1);
}

TEST_CASE("a guide link that names no page is an error") {
  LinkAudit links = make_checker();
  (void)links.resolve("99-nope.md", PAGES[0]);
  CHECK(links.errors().size() == 1);
}

TEST_CASE("site links are checked, with directories and the root") {
  LinkAudit links = make_checker(
      {"playground/", "ja/playground/", "guide/", "guide/introduction.html"});
  CHECK(links.resolve("../playground/", PAGES[0]) == "../playground/");
  CHECK(links.resolve("introduction.html", PAGES[0]) == "introduction.html");
  CHECK(links.resolve("../", PAGES[0]) == "../");
  CHECK(links.resolve("../ja/playground/", PAGES[0]) == "../ja/playground/");
  CHECK(links.errors().empty());
}

TEST_CASE("a site-root link is rebased onto the tree and the page") {
  LinkAudit links = make_checker(
      {"", "playground/", "ja/playground/", "ja/guide/sub/more.html"});
  CHECK(links.resolve("/playground/", PAGES[0]) == "../playground/");
  CHECK(links.resolve("/", PAGES[0]) == "../");
  CHECK(links.resolve("/playground/", PAGES[2]) == "../../playground/");
  CHECK(links.resolve("/playground/", PAGES[3]) == "../playground/");
  CHECK(links.resolve("/playground/", PAGES[5]) == "../../playground/");
  CHECK(links.resolve("/guide/sub/more.html#x", PAGES[3]) == "sub/more.html#x");
  CHECK(links.errors().empty());
}

TEST_CASE("a missing site target is an error") {
  LinkAudit links = make_checker();
  CHECK(links.resolve("../playground/", PAGES[0]) == "../playground/");
  CHECK(links.errors().size() == 1);
}

TEST_CASE("external, anchor, and mailto links pass through") {
  LinkAudit links = make_checker();
  for (const std::string_view url : {"https://example.com/x", "//example.com/x",
                                     "#heading", "mailto:a@example.com"}) {
    CHECK(links.resolve(url, PAGES[0]) == url);
  }
  CHECK(links.errors().empty());
}
