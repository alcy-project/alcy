// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/highlights.h"

#include <vector>

#include "doctest/doctest.h"
#include "fpag/base/numeric.h"
#include "lsp/analysis.h"
#include "lsp/document.h"
#include "text/json.h"

namespace lsp {

namespace {

struct Start {
  u32 line = 0;
  u32 character = 0;
};

std::vector<Start> starts(const text::JsonValue& highlights) {
  std::vector<Start> out;
  for (const text::JsonValue& highlight : highlights.as_array()) {
    const text::JsonValue* range = highlight.find("range");
    if (range == nullptr) {
      continue;
    }
    const text::JsonValue* start = range->find("start");
    out.push_back(
        Start{static_cast<u32>(start->find("line")->as_number()),
              static_cast<u32>(start->find("character")->as_number())});
  }
  return out;
}

bool has(const std::vector<Start>& found, u32 line, u32 character) {
  for (const Start& start : found) {
    if (start.line == line && start.character == character) {
      return true;
    }
  }
  return false;
}

// The highlight kind at a start position, or 0 when there is no highlight.
f64 kind_at(const text::JsonValue& highlights, u32 line, u32 character) {
  for (const text::JsonValue& highlight : highlights.as_array()) {
    const text::JsonValue* start = highlight.find("range")->find("start");
    if (static_cast<u32>(start->find("line")->as_number()) == line &&
        static_cast<u32>(start->find("character")->as_number()) == character) {
      return highlight.find("kind")->as_number();
    }
  }
  return 0;
}

}  // namespace

TEST_CASE("A function highlights its declaration and its call") {
  const Document document("file:///main.al", "alcy", 1,
                          "fn id(x: i32) -> i32 {\n  ret x\n}\nfn main() -> "
                          "i32 {\n  ret id(1)\n}\n");
  const DocumentAnalysis analysis(document);
  const std::vector<Start> found =
      starts(document_highlights(document, analysis, Position{4, 7}));
  CHECK(found.size() == 2);
  CHECK(has(found, 0, 3));
  CHECK(has(found, 4, 6));
}

TEST_CASE("A local highlights its binding and its use") {
  const Document document(
      "file:///main.al", "alcy", 1,
      "fn main() -> i32 {\n  x := 1\n  y := x\n  ret y\n}\n");
  const DocumentAnalysis analysis(document);
  const std::vector<Start> found =
      starts(document_highlights(document, analysis, Position{2, 7}));
  CHECK(found.size() == 2);
  CHECK(has(found, 1, 2));
  CHECK(has(found, 2, 7));
}

TEST_CASE("Linked editing joins the declaration and its uses") {
  const Document document("file:///main.al", "alcy", 1,
                          "fn id(x: i32) -> i32 {\n  ret x\n}\nfn main() -> "
                          "i32 {\n  ret id(1)\n}\n");
  const DocumentAnalysis analysis(document);
  const text::JsonValue linked =
      linked_editing_ranges(document, analysis, Position{4, 7});
  CHECK(linked.is_object());
  const text::JsonValue* ranges = linked.find("ranges");
  CHECK(ranges != nullptr);
  if (ranges != nullptr) {
    CHECK(ranges->as_array().size() == 2);
  }
  CHECK(linked.find("wordPattern") != nullptr);
}

TEST_CASE("An assignment highlights the write apart from reads") {
  const Document document(
      "file:///main.al", "alcy", 1,
      "fn main() -> i32 {\n  mut n := 0\n  n = n + 1\n  ret n\n}\n");
  const DocumentAnalysis analysis(document);
  const text::JsonValue highlights =
      document_highlights(document, analysis, Position{2, 2});
  CHECK(kind_at(highlights, 1, 6) == 2.0);  // declaration: Read
  CHECK(kind_at(highlights, 2, 2) == 3.0);  // assignment target: Write
  CHECK(kind_at(highlights, 2, 6) == 2.0);  // right-hand side: Read
  CHECK(kind_at(highlights, 3, 6) == 2.0);  // return: Read
}

TEST_CASE("A field assignment target highlights as a write") {
  const Document document("file:///main.al", "alcy", 1,
                          "struct Point {\n  x: i32,\n}\n"
                          "fn main() -> i32 {\n  mut p := Point { x: 1 }\n"
                          "  p.x = 2\n  ret p.x\n}\n");
  const DocumentAnalysis analysis(document);
  const text::JsonValue highlights =
      document_highlights(document, analysis, Position{5, 2});
  CHECK(kind_at(highlights, 5, 2) == 3.0);  // field write
  CHECK(kind_at(highlights, 6, 6) == 2.0);  // read
}

TEST_CASE("A comment highlights nothing") {
  const Document document("file:///main.al", "alcy", 1,
                          "// nothing\nfn main() -> i32 {\n  ret 0\n}\n");
  const DocumentAnalysis analysis(document);
  CHECK(document_highlights(document, analysis, Position{0, 3})
            .as_array()
            .empty());
}

}  // namespace lsp
