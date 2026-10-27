// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/folding.h"

#include <string>

#include "doctest/doctest.h"
#include "lsp/document.h"
#include "text/json.h"

namespace lsp {

namespace {

const text::JsonValue* find_range(const text::JsonValue& ranges,
                                  u32 start,
                                  u32 end) {
  for (const text::JsonValue& range : ranges.as_array()) {
    const text::JsonValue* line = range.find("startLine");
    const text::JsonValue* end_line = range.find("endLine");
    if (line != nullptr && end_line != nullptr &&
        line->as_number() == static_cast<f64>(start) &&
        end_line->as_number() == static_cast<f64>(end)) {
      return &range;
    }
  }
  return nullptr;
}

}  // namespace

TEST_CASE("A multi-line brace pair becomes a fold") {
  const Document document("file:///main.al", "alcy", 1,
                          "fn main() -> i32 {\n  if true {\n    ret 0\n"
                          "  }\n  ret 1\n}\n");
  const text::JsonValue ranges = document_folding_ranges(document);
  CHECK(ranges.is_array());
  CHECK(find_range(ranges, 0, 5) != nullptr);
  CHECK(find_range(ranges, 1, 3) != nullptr);
}

TEST_CASE("A single-line brace pair is not a fold") {
  const Document document("file:///one.al", "alcy", 1,
                          "fn f() -> i32 { ret 0 }\n");
  CHECK(document_folding_ranges(document).as_array().empty());
}

TEST_CASE("A run of comments becomes one fold") {
  const Document document("file:///c.al", "alcy", 1,
                          "// one\n// two\n// three\n"
                          "fn main() -> i32 {\n  ret 0\n}\n");
  const text::JsonValue ranges = document_folding_ranges(document);
  const text::JsonValue* comments = find_range(ranges, 0, 2);
  CHECK(comments != nullptr);
  if (comments != nullptr) {
    const text::JsonValue* kind = comments->find("kind");
    CHECK(kind != nullptr);
    if (kind != nullptr) {
      CHECK(kind->as_string() == "comment");
    }
  }
  CHECK(find_range(ranges, 3, 5) != nullptr);
}

}  // namespace lsp
