// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/signature.h"

#include "doctest/doctest.h"
#include "lsp/document.h"
#include "text/json.h"

namespace lsp {

namespace {

const char* const SOURCE =
    "fn add(a: i32, b: i32) -> i32 {\n"
    "  ret a\n"
    "}\n"
    "fn main() -> i32 {\n"
    "  ret add(1, 2)\n"
    "}\n";

}  // namespace

TEST_CASE("Signature help names the call and starts at the first parameter") {
  const Document document("file:///main.al", "alcy", 1, SOURCE);
  const text::JsonValue result = signature_help(document, Position{4, 10});
  CHECK(result.is_object());
  if (!result.is_object()) {
    return;
  }
  const text::JsonValue* signatures = result.find("signatures");
  CHECK(signatures != nullptr);
  if (signatures != nullptr) {
    const text::JsonValue& signature = signatures->as_array().front();
    const text::JsonValue* label = signature.find("label");
    CHECK(label != nullptr);
    if (label != nullptr) {
      CHECK(label->as_string() == "fn add(i32, i32) -> i32");
    }
  }
  const text::JsonValue* active = result.find("activeParameter");
  CHECK(active != nullptr);
  if (active != nullptr) {
    CHECK(active->as_number() == 0.0);
  }
}

TEST_CASE("Signature help follows the argument the cursor is in") {
  const Document document("file:///main.al", "alcy", 1, SOURCE);
  const text::JsonValue result = signature_help(document, Position{4, 13});
  CHECK(result.is_object());
  if (!result.is_object()) {
    return;
  }
  const text::JsonValue* active = result.find("activeParameter");
  CHECK(active != nullptr);
  if (active != nullptr) {
    CHECK(active->as_number() == 1.0);
  }
}

TEST_CASE("Signature help outside a call answers nothing") {
  const Document document("file:///main.al", "alcy", 1, SOURCE);
  CHECK(!signature_help(document, Position{1, 5}).is_object());
}

}  // namespace lsp
