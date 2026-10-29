// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/type_hierarchy.h"

#include "doctest/doctest.h"
#include "fpag/base/numeric.h"
#include "lsp/analysis.h"
#include "lsp/document.h"
#include "text/json.h"

namespace lsp {

namespace {

const char* const SOURCE =
    "spec Shape {\n"
    "  fn area(self: &Self) -> i32;\n"
    "}\n"
    "struct Square {\n"
    "  side: i32,\n"
    "}\n"
    "impl Shape for Square {\n"
    "  fn area(self: &Self) -> i32 {\n"
    "    ret self.side\n"
    "  }\n"
    "}\n";

}  // namespace

TEST_CASE("A struct prepares a type hierarchy item") {
  const Document document("file:///main.al", "alcy", 1, SOURCE);
  const DocumentAnalysis analysis(document);
  const text::JsonValue items =
      prepare_type_hierarchy(document, analysis, Position{3, 8});
  CHECK(items.as_array().size() == 1);
  if (!items.as_array().empty()) {
    CHECK(items.as_array().front().find("name")->as_string() == "Square");
    CHECK(items.as_array().front().find("kind")->as_number() == 23);
  }
}

TEST_CASE("A struct's supertypes are the specs it implements") {
  const Document document("file:///main.al", "alcy", 1, SOURCE);
  const DocumentAnalysis analysis(document);
  const text::JsonValue items =
      prepare_type_hierarchy(document, analysis, Position{3, 8});
  CHECK(!items.as_array().empty());
  if (items.as_array().empty()) {
    return;
  }
  const text::JsonValue supers =
      supertypes(document, analysis, items.as_array().front());
  CHECK(supers.as_array().size() == 1);
  if (!supers.as_array().empty()) {
    CHECK(supers.as_array().front().find("name")->as_string() == "Shape");
    CHECK(supers.as_array().front().find("kind")->as_number() == 11);
  }
}

TEST_CASE("A spec's subtypes are the types that implement it") {
  const Document document("file:///main.al", "alcy", 1, SOURCE);
  const DocumentAnalysis analysis(document);
  const text::JsonValue items =
      prepare_type_hierarchy(document, analysis, Position{0, 6});
  CHECK(!items.as_array().empty());
  if (items.as_array().empty()) {
    return;
  }
  CHECK(items.as_array().front().find("kind")->as_number() == 11);
  const text::JsonValue subs =
      subtypes(document, analysis, items.as_array().front());
  CHECK(subs.as_array().size() == 1);
  if (!subs.as_array().empty()) {
    CHECK(subs.as_array().front().find("name")->as_string() == "Square");
  }
}

TEST_CASE("A function name has no type hierarchy") {
  const Document document("file:///main.al", "alcy", 1,
                          "fn main() -> i32 {\n  ret 0\n}\n");
  const DocumentAnalysis analysis(document);
  CHECK(prepare_type_hierarchy(document, analysis, Position{0, 4})
            .as_array()
            .empty());
}

}  // namespace lsp
