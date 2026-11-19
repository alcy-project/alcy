// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "doctest/doctest.h"
#include "ssg/template.h"
#include "ssg/urls.h"
#include "tests/ssg/cases.h"
using ssg::canonical_url;
using ssg::speculation_rules;

namespace {}  // namespace

TEST_CASE("catalog strings are stamped in the page language") {
  auto ja = stamp_template("<h1>{{t:site.guide}}</h1>",
                           ssg_test::test_catalog(), "ja", {});
  auto en = stamp_template("<h1>{{t:site.guide}}</h1>",
                           ssg_test::test_catalog(), "en", {});
  CHECK(ja.is_ok());
  CHECK(en.is_ok());
  CHECK(std::move(ja).unwrap() ==
        "<h1>\xe3\x82\xac\xe3\x82\xa4\xe3\x83\x89</h1>");
  CHECK(std::move(en).unwrap() == "<h1>Guide</h1>");
}

TEST_CASE("computed values are inserted") {
  std::unordered_map<std::string, std::string> values = {
      {"home", "./"},
      {"lang", "ja"},
  };
  auto stamped = stamp_template("<a href=\"{{home}}guide/\">{{lang}}</a>",
                                ssg_test::test_catalog(), "ja", values);
  CHECK(stamped.is_ok());
  CHECK(std::move(stamped).unwrap() == "<a href=\"./guide/\">ja</a>");
}

TEST_CASE("an unknown message key fails the build") {
  const auto stamped =
      stamp_template("{{t:no.such.key}}", ssg_test::test_catalog(), "en", {});
  CHECK(stamped.is_err());
}

TEST_CASE("an unknown placeholder fails the build") {
  const auto stamped =
      stamp_template("{{nope}}", ssg_test::test_catalog(), "en", {});
  CHECK(stamped.is_err());
}

TEST_CASE("an index route is named at its directory") {
  CHECK(canonical_url("index.html") == "https://alcy-project.github.io/alcy/");
  CHECK(canonical_url("ja/guide/index.html") ==
        "https://alcy-project.github.io/alcy/ja/guide/");
  CHECK(canonical_url("guide/x.html") ==
        "https://alcy-project.github.io/alcy/guide/x.html");
}

TEST_CASE("every speculation pattern carries the published base") {
  const std::string rules = speculation_rules();
  CHECK(rules.find("/alcy/guide/**") != std::string::npos);
  CHECK(rules.find("/alcy/reference/**") != std::string::npos);
  CHECK(rules.find("/alcy/*") != std::string::npos);
  CHECK(rules.find("href_matches") != std::string::npos);
}
