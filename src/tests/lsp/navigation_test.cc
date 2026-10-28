// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/navigation.h"

#include <optional>
#include <string>
#include <vector>

#include "diag/span.h"
#include "doctest/doctest.h"
#include "lsp/analysis.h"
#include "lsp/document.h"

namespace lsp {

namespace {

const char* const SOURCE =
    "struct Point {\n"
    "  x: i32,\n"
    "}\n"
    "\n"
    "fn add(a: i32, b: i32) -> i32 {\n"
    "  ret a\n"
    "}\n"
    "\n"
    "fn take(p: Point) -> i32 {\n"
    "  ret p.x\n"
    "}\n"
    "\n"
    "fn main() -> i32 {\n"
    "  q := add(1, 2)\n"
    "  ret q\n"
    "}\n";

Position position_of(const Document& document, diag::Span span) {
  return document.index().position_at(document.text(), span.offset);
}

}  // namespace

TEST_CASE("A call resolves to the function declaration") {
  const Document document("file:///main.al", "alcy", 1, SOURCE);
  const DocumentAnalysis analysis(document);
  const std::optional<diag::Span> span =
      definition_at(document, analysis, Position{13, 8});
  CHECK(span.has_value());
  if (span.has_value()) {
    const Position start = position_of(document, *span);
    CHECK(start.line == 4);
    CHECK(start.character == 3);
  }
}

TEST_CASE("A function's references are its call sites") {
  const Document document("file:///main.al", "alcy", 1, SOURCE);
  const DocumentAnalysis analysis(document);
  const std::vector<diag::Span> sites =
      references_at(document, analysis, Position{4, 4});
  CHECK(sites.size() == 1);
  if (sites.size() == 1) {
    const Position start = position_of(document, sites[0]);
    CHECK(start.line == 13);
    CHECK(start.character == 7);
  }
}

TEST_CASE("A type reference resolves to the declaration") {
  const Document document("file:///main.al", "alcy", 1, SOURCE);
  const DocumentAnalysis analysis(document);
  const std::optional<diag::Span> span =
      definition_at(document, analysis, Position{8, 12});
  CHECK(span.has_value());
  if (span.has_value()) {
    const Position start = position_of(document, *span);
    CHECK(start.line == 0);
    CHECK(start.character == 7);
  }
}

TEST_CASE("A local use resolves to its binding") {
  const Document document("file:///main.al", "alcy", 1, SOURCE);
  const DocumentAnalysis analysis(document);
  const std::optional<diag::Span> span =
      definition_at(document, analysis, Position{14, 6});
  CHECK(span.has_value());
  if (span.has_value()) {
    const Position start = position_of(document, *span);
    CHECK(start.line == 13);
    CHECK(start.character == 2);
  }
}

TEST_CASE("Nothing at a comment answers nothing") {
  const Document document("file:///main.al", "alcy", 1,
                          "// nothing here\nfn main() -> i32 {\n  ret 0\n}\n");
  const DocumentAnalysis analysis(document);
  CHECK(!definition_at(document, analysis, Position{0, 5}).has_value());
  CHECK(references_at(document, analysis, Position{0, 5}).empty());
}

}  // namespace lsp
