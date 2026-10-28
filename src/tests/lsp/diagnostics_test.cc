// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/diagnostics.h"

#include "doctest/doctest.h"
#include "lsp/document.h"
#include "text/json.h"

namespace lsp {

TEST_CASE("The pull report wraps the same diagnostics") {
  const Document document("file:///main.al", "alcy", 1,
                          "fn main( -> i32 {\n}\n");
  const text::JsonValue report = document_diagnostic_report(document);
  CHECK(report.is_object());
  const text::JsonValue* kind = report.find("kind");
  CHECK(kind != nullptr);
  if (kind != nullptr) {
    CHECK(kind->as_string() == "full");
  }
  const text::JsonValue* items = report.find("items");
  CHECK(items != nullptr);
  if (items != nullptr) {
    CHECK(!items->as_array().empty());
  }
}

}  // namespace lsp
