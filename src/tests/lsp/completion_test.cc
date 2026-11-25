// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/completion.h"

#include <string>

#include "doctest/doctest.h"
#include "lsp/analysis.h"
#include "lsp/document.h"
#include "text/json.h"

namespace lsp {

namespace {

const char* const SOURCE =
    "struct Point {\n"
    "  x: i32,\n"
    "}\n"
    "fn add(a: i32, b: i32) -> i32 {\n"
    "  ret a\n"
    "}\n";

const text::JsonValue* find_item(const text::JsonValue& list,
                                 const std::string& label) {
  const text::JsonValue* items = list.find("items");
  if (items == nullptr) {
    return nullptr;
  }
  for (const text::JsonValue& item : items->as_array()) {
    const text::JsonValue* name = item.find("label");
    if (name != nullptr && name->as_string() == label) {
      return &item;
    }
  }
  return nullptr;
}

}  // namespace

TEST_CASE("Completion offers a declared struct for its prefix") {
  const Document document("file:///main.al", "alcy", 1, SOURCE);
  const DocumentAnalysis analysis(document);
  const text::JsonValue list =
      completion_items(document, analysis, Position{0, 12});
  const text::JsonValue* point = find_item(list, "Point");
  CHECK(point != nullptr);
  if (point != nullptr) {
    const text::JsonValue* kind = point->find("kind");
    CHECK(kind != nullptr);
    if (kind != nullptr) {
      CHECK(kind->as_number() == 22.0);
    }
  }
  CHECK(find_item(list, "fn") == nullptr);
}

TEST_CASE("Completion offers a function for its prefix") {
  const Document document("file:///main.al", "alcy", 1, SOURCE);
  const DocumentAnalysis analysis(document);
  const text::JsonValue list =
      completion_items(document, analysis, Position{4, 7});
  const text::JsonValue* add = find_item(list, "add");
  CHECK(add != nullptr);
  if (add != nullptr) {
    const text::JsonValue* kind = add->find("kind");
    CHECK(kind != nullptr);
    if (kind != nullptr) {
      CHECK(kind->as_number() == 3.0);
    }
  }
}

TEST_CASE("Completion offers the lexer's keywords, not reserved words") {
  const Document document("file:///main.al", "alcy", 1, SOURCE);
  const DocumentAnalysis analysis(document);
  const text::JsonValue list =
      completion_items(document, analysis, Position{0, 0});
  CHECK(find_item(list, "let") == nullptr);
  CHECK(find_item(list, "async") == nullptr);
  CHECK(find_item(list, "where") == nullptr);
  CHECK(find_item(list, "i128") == nullptr);
  CHECK(find_item(list, "i32") != nullptr);
  CHECK(find_item(list, "bool") != nullptr);
  CHECK(find_item(list, "Self") != nullptr);
  CHECK(find_item(list, "package") != nullptr);
  CHECK(find_item(list, "test") == nullptr);
}

TEST_CASE("Completion offers a keyword for its prefix") {
  const Document document("file:///main.al", "alcy", 1, SOURCE);
  const DocumentAnalysis analysis(document);
  const text::JsonValue list =
      completion_items(document, analysis, Position{4, 5});
  const text::JsonValue* item = find_item(list, "ret");
  CHECK(item != nullptr);
  if (item != nullptr) {
    const text::JsonValue* kind = item->find("kind");
    CHECK(kind != nullptr);
    if (kind != nullptr) {
      CHECK(kind->as_number() == 14.0);
    }
  }
}

TEST_CASE("Variant completion inserts the qualified name") {
  const Document document("file:///main.al", "alcy", 1,
                          "enum Option {\n  Some,\n  None,\n}\n"
                          "fn f() -> i32 {\n  Som\n}\n");
  const DocumentAnalysis analysis(document);
  const text::JsonValue list =
      completion_items(document, analysis, Position{5, 5});
  const text::JsonValue* item = find_item(list, "Some");
  CHECK(item != nullptr);
  if (item == nullptr) {
    return;
  }
  const text::JsonValue* edit = item->find("textEdit");
  CHECK(edit != nullptr);
  if (edit != nullptr) {
    CHECK(edit->find("newText")->as_string() == "Option::Some");
    const text::JsonValue* range = edit->find("range");
    CHECK(range != nullptr);
    if (range != nullptr) {
      CHECK(range->find("start")->find("character")->as_number() == 2.0);
      CHECK(range->find("end")->find("character")->as_number() == 5.0);
    }
  }
}

TEST_CASE("A documented struct offers its doc comment in completion") {
  const Document document("file:///main.al", "alcy", 1,
                          "/// A point in 2D.\nstruct Point {\n  x: i32,\n}\n"
                          "fn f() -> i32 {\n  Poi\n}\n");
  const DocumentAnalysis analysis(document);
  const text::JsonValue list =
      completion_items(document, analysis, Position{5, 5});
  const text::JsonValue* item = find_item(list, "Point");
  CHECK(item != nullptr);
  if (item == nullptr) {
    return;
  }
  const text::JsonValue* doc = item->find("documentation");
  CHECK(doc != nullptr);
  if (doc != nullptr) {
    CHECK(doc->find("value")->as_string() == "A point in 2D.");
  }
}

TEST_CASE("Completion offers a method after a receiver dot") {
  const Document document(
      "file:///main.al", "alcy", 1,
      "fn main() -> i32 {\n  mut n := 10\n"
      "  for i in (2..=n).step_by(2) {\n    n = n + i\n  }\n"
      "  ret n\n}\n");
  const DocumentAnalysis analysis(document);
  const text::JsonValue list =
      completion_items(document, analysis, Position{2, 26});
  const text::JsonValue* item = find_item(list, "step_by");
  CHECK(item != nullptr);
  if (item != nullptr) {
    const text::JsonValue* edit = item->find("textEdit");
    CHECK(edit != nullptr);
    if (edit != nullptr) {
      CHECK(edit->find("newText")->as_string() == "step_by");
    }
  }
}

TEST_CASE("Completion offers locals and parameters with their types") {
  const Document document("file:///main.al", "alcy", 1,
                          "fn use_it(v: i32) -> i32 {\n"
                          "  apple := 1\n"
                          "  w := v\n"
                          "  ret apple\n"
                          "}\n"
                          "fn main() -> i32 {\n"
                          "  banana := 2\n"
                          "  ret use_it(banana)\n"
                          "}\n");
  const DocumentAnalysis analysis(document);
  const text::JsonValue list =
      completion_items(document, analysis, Position{3, 2});
  for (const char* name : {"apple", "v", "w"}) {
    const text::JsonValue* item = find_item(list, name);
    CHECK(item != nullptr);
    if (item != nullptr) {
      const text::JsonValue* detail = item->find("detail");
      CHECK(detail != nullptr);
      if (detail != nullptr) {
        CHECK(detail->as_string() == "i32");
      }
    }
  }
  CHECK(find_item(list, "banana") == nullptr);
}

TEST_CASE("Completion survives an unfinished member access") {
  const Document document("file:///main.al", "alcy", 1,
                          "struct Foo {\n"
                          "  x: i32,\n"
                          "}\n"
                          "impl Foo {\n"
                          "  fn bar(self: Foo) -> i32 {\n"
                          "    ret self.x\n"
                          "  }\n"
                          "}\n"
                          "fn main() -> i32 {\n"
                          "  mut f := Foo{x: 1}\n"
                          "  f.\n"
                          "  ret 0\n"
                          "}\n");
  const DocumentAnalysis analysis(document);
  const text::JsonValue list =
      completion_items(document, analysis, Position{10, 4});
  CHECK(find_item(list, "bar") != nullptr);
}

TEST_CASE("Qualified variant completion replaces the whole path") {
  const Document document("file:///main.al", "alcy", 1,
                          "enum Option {\n  Some,\n  None,\n}\n"
                          "fn f() -> i32 {\n  Option::Som\n}\n");
  const DocumentAnalysis analysis(document);
  const text::JsonValue list =
      completion_items(document, analysis, Position{5, 13});
  const text::JsonValue* item = find_item(list, "Some");
  CHECK(item != nullptr);
  if (item == nullptr) {
    return;
  }
  const text::JsonValue* edit = item->find("textEdit");
  CHECK(edit != nullptr);
  if (edit == nullptr) {
    return;
  }
  CHECK(edit->find("newText")->as_string() == "Option::Some");
  const text::JsonValue* range = edit->find("range");
  CHECK(range != nullptr);
  if (range != nullptr) {
    CHECK(range->find("start")->find("character")->as_number() == 2.0);
    CHECK(range->find("end")->find("character")->as_number() == 13.0);
  }
}

TEST_CASE("Completion carries a text edit and doc comments") {
  const Document document("file:///main.al", "alcy", 1,
                          "/// Adds two numbers.\n"
                          "fn add(a: i32, b: i32) -> i32 {\n"
                          "  ret a\n"
                          "}\n"
                          "fn f() -> i32 {\n"
                          "  ad\n"
                          "}\n");
  const DocumentAnalysis analysis(document);
  const text::JsonValue list =
      completion_items(document, analysis, Position{5, 4});
  const text::JsonValue* item = find_item(list, "add");
  CHECK(item != nullptr);
  if (item == nullptr) {
    return;
  }
  const text::JsonValue* text_edit = item->find("textEdit");
  CHECK(text_edit != nullptr);
  if (text_edit != nullptr) {
    CHECK(text_edit->find("newText")->as_string() == "add");
  }
  const text::JsonValue* documentation = item->find("documentation");
  CHECK(documentation != nullptr);
  if (documentation != nullptr) {
    CHECK(documentation->find("value")->as_string() == "Adds two numbers.");
  }
}
}  // namespace lsp
