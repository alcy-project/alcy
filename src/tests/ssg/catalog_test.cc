// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "ssg/catalog.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "doctest/doctest.h"
#include "fpag/io/temp_dir.h"
#include "ssg/documents.h"
#include "ssg/render.h"
#include "ssg/shell.h"
#include "ssg/urls.h"
#include "tests/ssg/cases.h"
using ssg::Catalog;
using ssg::DocPage;
using ssg::SettingsOptions;
using ssg::Stamp;
using ssg::translate;

namespace {

const Stamp STAMP{.text = "TESTSTAMP",
                  .url = "https://example.test/commit/TESTSTAMP"};

std::string settings_html(std::string_view language,
                          std::string_view route,
                          std::string_view tree_path) {
  const ssg::Catalog& catalog = ssg_test::test_catalog();
  std::vector<ssg::LanguageOption> languages;
  for (const std::string& target : catalog.languages) {
    const ssg::LanguageInfo info = ssg::language_info_of(catalog, target);
    languages.push_back(
        {.language = target,
         .name = info.name,
         .href = ssg::language_href(catalog, route, target, tree_path),
         .current = target == language});
  }
  ssg::SettingsOptions options;
  options.catalog = &catalog;
  options.language = language;
  options.languages = std::move(languages);
  return ssg::render_settings(options);
}

// A scratch directory load_catalog reads: fpag's TempDir wipes and
// recreates it on construction and removes it on destruction, so
// the cases here cannot leak state into one another.
using TempDir = io::TempDir;

void write_file(io::TempDir& dir,
                std::string_view rel,
                std::string_view content) {
  CHECK(dir.write_file(rel, content));
}
}  // namespace

TEST_CASE(
    "the settings menu carries the theme toggle and a language dropdown") {
  const std::string html =
      settings_html("ja", "ja/guide/x.html", "guide/x.html");
  CHECK(html.find("id=\"theme\"") != std::string::npos);
  CHECK(html.find("data-theme-mode=\"auto\"") != std::string::npos);
  CHECK(html.find("class=\"language-menu\"") != std::string::npos);
  CHECK(html.find("href=\"../../guide/x.html\" hreflang=\"en\" lang=\"en\"") !=
        std::string::npos);
  CHECK(html.find("href=\"x.html\" hreflang=\"ja\" lang=\"ja\" "
                  "aria-current=\"true\"") != std::string::npos);
  CHECK(html.find("English") != std::string::npos);
  CHECK(html.find("aria-label=\"" +
                  translate(ssg_test::test_catalog(), "ja", "label.settings") +
                  "\"") != std::string::npos);
}

TEST_CASE("the menu icon is stroked, so it is visible") {
  const std::string html = settings_html("en", "index.html", "index.html");
  CHECK(html.find("<summary") != std::string::npos);
  CHECK(html.find("stroke=\"currentColor\"") != std::string::npos);
}

TEST_CASE("a page names the catalog of its own tree") {
  const std::string en = ssg_test::render_article({}, ssg::GUIDE_TREE, "en",
                                                  {ssg_test::prose_block()});
  const std::string ja = ssg_test::render_article({}, ssg::GUIDE_TREE, "ja",
                                                  {ssg_test::prose_block()});
  CHECK(en.find("shared/i18n.en.js") != std::string::npos);
  CHECK(ja.find("shared/i18n.ja.js") != std::string::npos);
}

TEST_CASE("the tree index names the catalog of the tree it lists") {
  ssg::DocPage page;
  page.source_rel = "01-x.md";
  page.path = "guide/x.html";
  page.order = 1;
  page.title = "x";
  page.language = "en";
  page.route = page.path;
  const std::vector<DocPage> pages = {page};
  const std::string en = ssg::render_tree_index(
      pages, ssg_test::test_catalog(), "en", "BOOT", STAMP, ssg::GUIDE_TREE);
  const std::string ja = ssg::render_tree_index(
      pages, ssg_test::test_catalog(), "ja", "BOOT", STAMP, ssg::GUIDE_TREE);
  CHECK(en.find("shared/i18n.en.js") != std::string::npos);
  CHECK(ja.find("shared/i18n.ja.js") != std::string::npos);
}

TEST_CASE("the not-found page names a catalog too") {
  const std::string page = ssg::render_not_found(
      "BOOT", STAMP, [](std::string_view code) { return std::string(code); });
  CHECK(page.find("shared/i18n.en.js") != std::string::npos);
}

TEST_CASE("the root tree's catalog map is a resolvable URL") {
  const std::string map = ssg::catalog_map("", "en");
  CHECK(map.find("\"alcy:i18n\":\"./shared/i18n.en.js\"") != std::string::npos);
}

TEST_CASE("the map comes before the first module the page loads") {
  for (const std::string_view language : {"en", "ja"}) {
    const std::string html = ssg_test::render_article(
        {}, ssg::GUIDE_TREE, language, {ssg_test::prose_block()});
    const usize map = html.find("<script type=\"importmap\">");
    const usize load = html.find("type=\"module\"");
    CHECK(map != std::string::npos);
    CHECK(load != std::string::npos);
    CHECK(map < load);
  }
}

TEST_CASE(
    "a language the loaded catalog lacks falls back instead of throwing") {
  const Catalog& catalog = ssg_test::test_catalog();
  CHECK(translate(catalog, "fr", "site.guide") == "Guide");
  CHECK(translate(catalog, "ja", "site.guide") ==
        "\xe3\x82\xac\xe3\x82\xa4\xe3\x83\x89");
}

TEST_CASE("a page stamp names the build and links to it") {
  CHECK(ssg::footer({.text = "TESTSTAMP",
                     .url = "https://example.test/commit/TESTSTAMP"})
            .find("<a href=\"https://example.test/commit/TESTSTAMP\" "
                  "rel=\"noopener\">TESTSTAMP</a>") != std::string::npos);
}

TEST_CASE("the header names all five destinations") {
  const std::string html = ssg::site_header(
      ssg_test::test_catalog(), "index.html", "en", "index.html", "");
  for (const std::string& href :
       {"./guide/", "./reference/", "./tools/", "./playground/"}) {
    CHECK(html.find(href) != std::string::npos);
  }
  CHECK(html.find("data-action=\"search\"") != std::string::npos);
  CHECK(html.find("aria-current=\"page\"") == std::string::npos);
}

TEST_CASE("a tree page marks its own tree current") {
  const std::string html =
      ssg::site_header(ssg_test::test_catalog(), "reference/ownership.html",
                       "en", "reference/ownership.html", "reference");
  CHECK(html.find("aria-current=\"page\">Reference</a>") != std::string::npos);
}

TEST_CASE("the playground marks itself current") {
  const std::string html =
      ssg::site_header(ssg_test::test_catalog(), "playground/index.html", "en",
                       "playground/index.html", "playground");
  CHECK(html.find("aria-current=\"page\">Playground</a>") != std::string::npos);
}

TEST_CASE("the Japanese header stamps Japanese labels") {
  const std::string html = ssg::site_header(
      ssg_test::test_catalog(), "ja/index.html", "ja", "index.html", "");
  for (const std::string_view label :
       {"\xe3\x82\xac\xe3\x82\xa4\xe3\x83\x89",
        "\xe3\x83\xaa\xe3\x83\x95\xe3\x82\xa1\xe3\x83\xac\xe3\x83\xb3\xe3\x82"
        "\xb9",
        "\xe3\x83\x84\xe3\x83\xbc\xe3\x83\xab",
        "\xe3\x83\x97\xe3\x83\xac\xe3\x82\xa4\xe3\x82\xb0\xe3\x83\xa9\xe3\x82"
        "\xa6\xe3\x83\xb3\xe3\x83\x89",
        "\xe6\xa4\x9c\xe7\xb4\xa2"}) {
    CHECK(html.find(label) != std::string::npos);
  }
}

TEST_CASE("load_catalog refuses a message with no default counterpart") {
  io::TempDir dir("catalog-counterpart");
  write_file(dir, "catalog.json",
             R"({"languages": ["en", "ja"], "default": "en",
         "names": {"en": "English", "ja": "Japanese"},
         "messages": {"en": {"a": "A"}, "ja": {"a": "A", "b": "B"}}})");
  auto loaded = ssg::load_catalog(std::string(dir.join("catalog.json")));
  CHECK(loaded.is_err());
  const std::string error = std::move(loaded).unwrap_err();
  CHECK_MESSAGE(error.find("has no en counterpart") != std::string::npos,
                error);
}

TEST_CASE("load_catalog refuses an unknown top-level key") {
  io::TempDir dir("catalog-key");
  write_file(dir, "catalog.json", R"({"languages": ["en"], "surprise": {}})");
  auto loaded = ssg::load_catalog(std::string(dir.join("catalog.json")));
  CHECK(loaded.is_err());
  const std::string error = std::move(loaded).unwrap_err();
  CHECK_MESSAGE(error.find("unknown top-level key") != std::string::npos,
                error);
}

TEST_CASE("load_catalog refuses junk after the closing brace") {
  io::TempDir dir("catalog-junk");
  write_file(dir, "catalog.json",
             R"json({"languages": ["en"]}) and nothing else")json");
  auto loaded = ssg::load_catalog(std::string(dir.join("catalog.json")));
  CHECK(loaded.is_err());
  const std::string error = std::move(loaded).unwrap_err();
  CHECK_MESSAGE(error.find("junk after") != std::string::npos, error);
}

TEST_CASE("load_catalog refuses a surrogate escape") {
  io::TempDir dir("catalog-surrogate");
  write_file(dir, "catalog.json",
             R"({"languages": ["en"], "default": "en",
         "names": {"en": "Engli\ud83dsh"},
         "messages": {"en": {"a": "A"}}})");
  auto loaded = ssg::load_catalog(std::string(dir.join("catalog.json")));
  CHECK(loaded.is_err());
  const std::string error = std::move(loaded).unwrap_err();
  CHECK_MESSAGE(error.find("surrogate") != std::string::npos, error);
}
