// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/call_hierarchy.h"

#include "doctest/doctest.h"
#include "lsp/analysis.h"
#include "lsp/document.h"
#include "text/json.h"

namespace lsp {

namespace {

const char* const SOURCE =
    "fn id(x: i32) -> i32 {\n  ret x\n}\nfn main() -> i32 {\n  ret id(1)\n}\n";

}  // namespace

TEST_CASE("A function prepares a call hierarchy item") {
  const Document document("file:///main.al", "alcy", 1, SOURCE);
  const DocumentAnalysis analysis(document);
  const text::JsonValue items =
      prepare_call_hierarchy(document, analysis, Position{0, 4});
  CHECK(items.as_array().size() == 1);
  if (!items.as_array().empty()) {
    CHECK(items.as_array().front().find("name")->as_string() == "id");
    CHECK(items.as_array().front().find("kind")->as_number() == 12);
  }
}

TEST_CASE("A function's incoming calls name the caller") {
  const Document document("file:///main.al", "alcy", 1, SOURCE);
  const DocumentAnalysis analysis(document);
  const text::JsonValue items =
      prepare_call_hierarchy(document, analysis, Position{0, 4});
  CHECK(!items.as_array().empty());
  if (items.as_array().empty()) {
    return;
  }
  const text::JsonValue calls =
      incoming_calls(document, analysis, items.as_array().front());
  CHECK(calls.as_array().size() == 1);
  if (!calls.as_array().empty()) {
    CHECK(calls.as_array().front().find("from")->find("name")->as_string() ==
          "main");
    CHECK(calls.as_array().front().find("fromRanges")->as_array().size() == 1);
  }
}

TEST_CASE("A function's outgoing calls name the callee") {
  const Document document("file:///main.al", "alcy", 1, SOURCE);
  const DocumentAnalysis analysis(document);
  const text::JsonValue items =
      prepare_call_hierarchy(document, analysis, Position{3, 4});
  CHECK(!items.as_array().empty());
  if (items.as_array().empty()) {
    return;
  }
  const text::JsonValue calls =
      outgoing_calls(document, analysis, items.as_array().front());
  CHECK(calls.as_array().size() == 1);
  if (!calls.as_array().empty()) {
    CHECK(calls.as_array().front().find("to")->find("name")->as_string() ==
          "id");
    CHECK(calls.as_array().front().find("fromRanges")->as_array().size() == 1);
  }
}

TEST_CASE("A local has no call hierarchy") {
  const Document document("file:///main.al", "alcy", 1,
                          "fn main() -> i32 {\n  x := 1\n  ret x\n}\n");
  const DocumentAnalysis analysis(document);
  CHECK(prepare_call_hierarchy(document, analysis, Position{2, 6})
            .as_array()
            .empty());
}

}  // namespace lsp
