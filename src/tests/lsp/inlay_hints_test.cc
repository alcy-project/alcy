// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/inlay_hints.h"

#include <string>

#include "doctest/doctest.h"
#include "lsp/analysis.h"
#include "lsp/document.h"
#include "text/json.h"

namespace lsp {

namespace {

const text::JsonValue* find_hint(const text::JsonValue& hints,
                                 const std::string& label) {
  for (const text::JsonValue& hint : hints.as_array()) {
    const text::JsonValue* value = hint.find("label");
    if (value != nullptr && value->as_string() == label) {
      return &hint;
    }
  }
  return nullptr;
}

}  // namespace

TEST_CASE("A binding without a type shows the inferred type") {
  const Document document("file:///main.al", "alcy", 1,
                          "fn f() -> i32 {\n  x := 1\n  ret x\n}\n");
  const DocumentAnalysis analysis(document);
  const text::JsonValue hints = inlay_hints(document, analysis);
  CHECK(hints.is_array());
  CHECK(hints.as_array().size() == 1);
  const text::JsonValue* hint = find_hint(hints, ": i32");
  CHECK(hint != nullptr);
  if (hint != nullptr) {
    const text::JsonValue* position = hint->find("position");
    CHECK(position != nullptr);
    if (position != nullptr) {
      const text::JsonValue* line = position->find("line");
      const text::JsonValue* character = position->find("character");
      CHECK(line != nullptr);
      CHECK(character != nullptr);
      if (line != nullptr) {
        CHECK(line->as_number() == 1.0);
      }
      if (character != nullptr) {
        CHECK(character->as_number() == 3.0);
      }
    }
  }
}

TEST_CASE("A struct shows its size and each field its offset") {
  const Document document("file:///main.al", "alcy", 1,
                          "struct Point {\n  x: i32,\n  y: i64,\n}\n");
  const DocumentAnalysis analysis(document);
  const text::JsonValue hints = inlay_hints(document, analysis);
  CHECK(hints.as_array().size() == 3);
  CHECK(find_hint(hints, "size 16, align 8") != nullptr);
  CHECK(find_hint(hints, "offset 0") != nullptr);
  CHECK(find_hint(hints, "offset 8") != nullptr);
}

TEST_CASE("A call shows the parameter name before its argument") {
  const Document document("file:///main.al", "alcy", 1,
                          "fn area(w: i32, h: i32) -> i32 {\n  ret w\n}\nfn "
                          "main() -> i32 {\n  ret area(2, 3)\n}\n");
  const DocumentAnalysis analysis(document);
  const text::JsonValue hints = inlay_hints(document, analysis);
  const text::JsonValue* width = find_hint(hints, "w:");
  CHECK(width != nullptr);
  CHECK(find_hint(hints, "h:") != nullptr);
  if (width != nullptr) {
    CHECK(width->find("kind")->as_number() == 2.0);
  }
}

TEST_CASE("An argument that names its parameter gets no hint") {
  const Document document("file:///main.al", "alcy", 1,
                          "fn id(x: i32) -> i32 {\n  ret x\n}\nfn main() -> "
                          "i32 {\n  x := 1\n  ret id(x)\n}\n");
  const DocumentAnalysis analysis(document);
  const text::JsonValue hints = inlay_hints(document, analysis);
  CHECK(find_hint(hints, "x:") == nullptr);
}

TEST_CASE("An enum shows its size") {
  const Document document("file:///main.al", "alcy", 1,
                          "enum Shape {\n  A(i32),\n}\n");
  const DocumentAnalysis analysis(document);
  const text::JsonValue hints = inlay_hints(document, analysis);
  CHECK(hints.as_array().size() == 1);
  CHECK(find_hint(hints, "size 8, align 4") != nullptr);
}

}  // namespace lsp
