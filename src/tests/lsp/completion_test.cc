// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/completion.h"

#include <string>

#include "doctest/doctest.h"
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
  const text::JsonValue list = completion_items(document, Position{0, 12});
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
  const text::JsonValue list = completion_items(document, Position{4, 7});
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

TEST_CASE("Completion offers a keyword for its prefix") {
  const Document document("file:///main.al", "alcy", 1, SOURCE);
  const text::JsonValue list = completion_items(document, Position{4, 5});
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

}  // namespace lsp
