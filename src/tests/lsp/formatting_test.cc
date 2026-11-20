// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/formatting.h"

#include "doctest/doctest.h"
#include "lsp/document.h"
#include "text/json.h"

namespace lsp {

TEST_CASE("Range formatting edits only a changed span that intersects") {
  const Document document("file:///main.al", "alcy", 1, "struct  Point{x:i32}");
  const text::JsonValue edits =
      document_range_formatting(document, Position{0, 0}, Position{0, 8});
  CHECK(!edits.as_array().empty());

  const text::JsonValue far =
      document_range_formatting(document, Position{9, 0}, Position{10, 0});
  CHECK(far.as_array().empty());
}

TEST_CASE("Range formatting does not edit changes outside the range") {
  const std::string source = "fn  main() {\n  let  x = 1\n}\n";
  const Document document("file:///main.al", "alcy", 1, source);
  const usize first_line = source.find('\n');
  const text::JsonValue edits = document_range_formatting(
      document, Position{0, 0}, Position{0, static_cast<u32>(first_line)});
  for (const text::JsonValue& edit : edits.as_array()) {
    const text::JsonValue* range = edit.find("range");
    if (range == nullptr) {
      continue;
    }
    const text::JsonValue* start = range->find("start");
    if (start != nullptr) {
      CHECK(start->find("line")->as_number() == 0);
    }
  }
}

TEST_CASE("Range formatting emits one edit when the formatter adds lines") {
  const Document document("file:///main.al", "alcy", 1, "struct  Point{x:i32}");
  const text::JsonValue edits =
      document_range_formatting(document, Position{0, 0}, Position{0, 8});
  CHECK(edits.as_array().size() == 1);
}

TEST_CASE("Range formatting uses absolute offsets for later lines") {
  const Document document("file:///main.al", "alcy", 1,
                          "fn main() {\n  let  x = 1\n}\n");
  const text::JsonValue edits =
      document_range_formatting(document, Position{1, 0}, Position{1, 12});
  CHECK(edits.as_array().size() == 1);
  if (edits.as_array().empty()) {
    return;
  }
  const text::JsonValue* range = edits.as_array().front().find("range");
  const text::JsonValue* start = range->find("start");
  const text::JsonValue* end = range->find("end");
  CHECK(start->find("line")->as_number() == 1);
  CHECK(start->find("character")->as_number() == 6);
  CHECK(end->find("character")->as_number() == 7);
}

TEST_CASE("Range formatting leaves canonical text alone") {
  const Document document("file:///main.al", "alcy", 1,
                          "struct Point {\n  x: i32,\n}\n");
  const text::JsonValue edits =
      document_range_formatting(document, Position{0, 0}, Position{2, 1});
  CHECK(edits.as_array().empty());
}

}  // namespace lsp
