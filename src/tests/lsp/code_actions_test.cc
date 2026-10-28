// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/code_actions.h"

#include <string>

#include "doctest/doctest.h"
#include "lsp/document.h"
#include "text/json.h"

namespace lsp {

TEST_CASE("A needless return offers one quick fix") {
  const Document document("file:///main.al", "alcy", 1,
                          "fn main() -> i32 {\n  ret 1\n}\n");
  const text::JsonValue actions =
      code_actions(document, Position{1, 0}, Position{1, 7});
  CHECK(actions.is_array());
  CHECK(actions.as_array().size() == 1);
  if (actions.as_array().empty()) {
    return;
  }
  const text::JsonValue& action = actions.as_array().front();
  const text::JsonValue* kind = action.find("kind");
  CHECK(kind != nullptr);
  if (kind != nullptr) {
    CHECK(kind->as_string() == "quickfix");
  }
  const text::JsonValue* edit = action.find("edit");
  CHECK(edit != nullptr);
  if (edit == nullptr) {
    return;
  }
  const text::JsonValue* changes = edit->find("changes");
  CHECK(changes != nullptr);
  if (changes == nullptr) {
    return;
  }
  const text::JsonValue* edits = changes->find(document.uri());
  CHECK(edits != nullptr);
  if (edits == nullptr || edits->as_array().empty()) {
    return;
  }
  const text::JsonValue& text_edit = edits->as_array().front();
  const text::JsonValue* range = text_edit.find("range");
  const text::JsonValue* replacement = text_edit.find("newText");
  CHECK(range != nullptr);
  CHECK(replacement != nullptr);
  if (range != nullptr) {
    const text::JsonValue* start = range->find("start");
    CHECK(start != nullptr);
    if (start != nullptr) {
      const text::JsonValue* character = start->find("character");
      CHECK(character != nullptr);
      if (character != nullptr) {
        CHECK(character->as_number() == 2.0);
      }
    }
  }
  if (replacement != nullptr) {
    CHECK(replacement->as_string().empty());
  }
}

TEST_CASE("A clean buffer offers no quick fix") {
  const Document document("file:///main.al", "alcy", 1,
                          "fn main() -> i32 {\n  1\n}\n");
  const text::JsonValue actions =
      code_actions(document, Position{0, 0}, Position{3, 0});
  CHECK(actions.is_array());
  CHECK(actions.as_array().empty());
}

}  // namespace lsp
