// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/diagnostics.h"

#include <string_view>
#include <vector>

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

TEST_CASE("The workspace report carries one entry per open document") {
  DocumentStore documents;
  documents.open("file:///ok.al", "alcy", 1,
                 "fn main() -> i32 {\n  ret 0\n}\n");
  documents.open("file:///bad.al", "alcy", 1, "fn main( -> i32 {\n}\n");
  const text::JsonValue report = workspace_diagnostic_report(documents);
  const text::JsonValue* items = report.find("items");
  CHECK(items != nullptr);
  if (items == nullptr) {
    return;
  }
  CHECK(items->as_array().size() == 2);
  bool broken = false;
  for (const text::JsonValue& item : items->as_array()) {
    const text::JsonValue* uri = item.find("uri");
    const text::JsonValue* diagnostics = item.find("items");
    if (uri != nullptr && diagnostics != nullptr &&
        uri->as_string() == "file:///bad.al" &&
        !diagnostics->as_array().empty()) {
      broken = true;
    }
  }
  CHECK(broken);
}

TEST_CASE("A repeated pull with the same id is unchanged") {
  DiagnosticsCache cache;
  const Document document("file:///main.al", "alcy", 1,
                          "fn main( -> i32 {\n}\n");
  const text::JsonValue first = cache.pull(document, {});
  CHECK(first.find("kind")->as_string() == "full");
  const std::string_view id = first.find("resultId")->as_string();
  const text::JsonValue second = cache.pull(document, id);
  CHECK(second.find("kind")->as_string() == "unchanged");
  CHECK(second.find("resultId")->as_string() == id);
}

TEST_CASE("A new version pulls a full report again") {
  DiagnosticsCache cache;
  Document document("file:///main.al", "alcy", 1, "fn main( -> i32 {\n}\n");
  const text::JsonValue first = cache.pull(document, {});
  const std::string_view id = first.find("resultId")->as_string();
  const std::vector<ContentChange> changes;
  document.apply(changes, 2);
  const text::JsonValue second = cache.pull(document, id);
  CHECK(second.find("kind")->as_string() == "full");
}

TEST_CASE("A stale id pulls a full report") {
  DiagnosticsCache cache;
  const Document document("file:///main.al", "alcy", 1,
                          "fn main( -> i32 {\n}\n");
  (void)cache.pull(document, {});
  const text::JsonValue second = cache.pull(document, "stale");
  CHECK(second.find("kind")->as_string() == "full");
}

}  // namespace lsp
