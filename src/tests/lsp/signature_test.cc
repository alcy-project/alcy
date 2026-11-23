// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/signature.h"

#include "doctest/doctest.h"
#include "lsp/analysis.h"
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
  const DocumentAnalysis analysis(document);
  const text::JsonValue result =
      signature_help(document, analysis, Position{4, 10});
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
      CHECK(label->as_string() == "fn add(a: i32, b: i32) -> i32");
    }
    const text::JsonValue* params = signature.find("parameters");
    CHECK(params != nullptr);
    if (params != nullptr && params->as_array().size() == 2) {
      CHECK(params->as_array()[0].find("label")->as_string() == "a: i32");
      CHECK(params->as_array()[1].find("label")->as_string() == "b: i32");
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
  const DocumentAnalysis analysis(document);
  const text::JsonValue result =
      signature_help(document, analysis, Position{4, 13});
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
  const DocumentAnalysis analysis(document);
  CHECK(!signature_help(document, analysis, Position{1, 5}).is_object());
}

TEST_CASE("Signature help shows a variadic parameter") {
  const Document document(
      "file:///main.al", "alcy", 1,
      "comp fn count(args: ..) -> usize {\n  ret args.len()\n}\n"
      "fn main() -> i32 {\n  ret count(1)\n}\n");
  const DocumentAnalysis analysis(document);
  const text::JsonValue result =
      signature_help(document, analysis, Position{4, 9});
  const text::JsonValue* signatures = result.find("signatures");
  CHECK(signatures != nullptr);
  if (signatures == nullptr || signatures->as_array().empty()) {
    return;
  }
  const text::JsonValue& signature = signatures->as_array().front();
  CHECK(signature.find("label")->as_string() == "fn count(args: ..) -> usize");
  const text::JsonValue* parameters = signature.find("parameters");
  CHECK(parameters != nullptr);
  if (parameters != nullptr) {
    CHECK(parameters->as_array().size() == 1);
  }
}
}  // namespace lsp
