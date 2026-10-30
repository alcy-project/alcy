// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/workspace_symbols.h"

#include <string>
#include <vector>

#include "doctest/doctest.h"
#include "lsp/document.h"
#include "text/json.h"

namespace lsp {

namespace {

std::vector<std::string> names(const text::JsonValue& symbols) {
  std::vector<std::string> out;
  for (const text::JsonValue& symbol : symbols.as_array()) {
    out.emplace_back(symbol.find("name")->as_string());
  }
  return out;
}

void open_two_documents(DocumentStore* documents) {
  documents->open("file:///a.al", "alcy", 1, "struct Point {\n  x: i32,\n}\n");
  documents->open("file:///b.al", "alcy", 1,
                  "fn area() -> i32 {\n  ret 0\n}\n");
}

}  // namespace

TEST_CASE("An empty query returns every open document's symbols") {
  DocumentStore documents;
  open_two_documents(&documents);
  const std::vector<std::string> found =
      names(workspace_symbols(documents, ""));
  CHECK(found.size() == 3);
}

TEST_CASE("A query matches a case-insensitive substring") {
  DocumentStore documents;
  open_two_documents(&documents);
  const std::vector<std::string> found =
      names(workspace_symbols(documents, "poi"));
  CHECK(found.size() == 1);
  if (found.size() == 1) {
    CHECK(found[0] == "Point");
  }
}

TEST_CASE("A nested symbol carries its container") {
  DocumentStore documents;
  open_two_documents(&documents);
  const text::JsonValue found = workspace_symbols(documents, "x");
  bool container = false;
  for (const text::JsonValue& symbol : found.as_array()) {
    if (symbol.find("name")->as_string() == "x" &&
        symbol.find("containerName") != nullptr) {
      container = true;
    }
  }
  CHECK(container);
}

TEST_CASE("A query matches by subsequence, not only by substring") {
  DocumentStore documents;
  open_two_documents(&documents);
  const std::vector<std::string> found =
      names(workspace_symbols(documents, "pn"));
  CHECK(found.size() == 1);
  if (found.size() == 1) {
    CHECK(found[0] == "Point");
  }
}

TEST_CASE("A query with no match answers nothing") {
  DocumentStore documents;
  open_two_documents(&documents);
  CHECK(workspace_symbols(documents, "zzz").as_array().empty());
}

}  // namespace lsp
