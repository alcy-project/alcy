// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/hover.h"

#include <optional>
#include <string>

#include "doctest/doctest.h"
#include "lsp/analysis.h"
#include "lsp/document.h"

namespace lsp {

namespace {

std::optional<std::string> hover(const std::string& text,
                                 u32 line,
                                 u32 character) {
  const Document document("file:///main.al", "alcy", 1, text);
  const DocumentAnalysis analysis(document);
  return hover_markdown(document, analysis, Position{line, character});
}

}  // namespace

TEST_CASE("Hover on a struct declaration shows its layout") {
  const std::optional<std::string> markdown =
      hover("struct Mix {\n  a: u8,\n  b: i32,\n}\n", 0, 8);
  CHECK(markdown.has_value());
  if (markdown.has_value()) {
    CHECK(markdown->find("size **8**") != std::string::npos);
  }
}

TEST_CASE("Hover on a field shows its place") {
  const std::optional<std::string> markdown =
      hover("struct Mix {\n  a: u8,\n  b: i32,\n}\n", 2, 2);
  CHECK(markdown.has_value());
  if (markdown.has_value()) {
    CHECK(markdown->find("Mix.b: i32") != std::string::npos);
    CHECK(markdown->find("offset 4") != std::string::npos);
  }
}

TEST_CASE("Hover on an enum declaration shows the payload area") {
  const std::optional<std::string> markdown =
      hover("enum Shape {\n  Dot,\n  Line(i32),\n}\n", 0, 6);
  CHECK(markdown.has_value());
  if (markdown.has_value()) {
    CHECK(markdown->find("payload at") != std::string::npos);
  }
}

TEST_CASE("Hover on a function shows its signature") {
  const std::optional<std::string> markdown =
      hover("fn add(a: i32, b: i32) -> i32 {\n  ret a\n}\n", 0, 4);
  CHECK(markdown.has_value());
  if (markdown.has_value()) {
    CHECK(markdown->find("fn add(a: i32, b: i32) -> i32") != std::string::npos);
  }
}

TEST_CASE("Hover shows the doc comments above a function") {
  const std::optional<std::string> markdown = hover(
      "/// Adds two numbers.\n"
      "/// Second line.\n"
      "fn add(a: i32, b: i32) -> i32 {\n"
      "  ret a\n"
      "}\n",
      2, 4);
  CHECK(markdown.has_value());
  if (markdown.has_value()) {
    CHECK(markdown->find("Adds two numbers.") != std::string::npos);
    CHECK(markdown->find("Second line.") != std::string::npos);
    CHECK(markdown->find("///") == std::string::npos);
  }
}

TEST_CASE("Hover shows the doc comments above a const") {
  const std::optional<std::string> markdown = hover(
      "/// The limit.\n"
      "const LIMIT: i32 := 3\n",
      1, 7);
  CHECK(markdown.has_value());
  if (markdown.has_value()) {
    CHECK(markdown->find("The limit.") != std::string::npos);
  }
}

TEST_CASE("Hover on a const shows its decided type") {
  const std::optional<std::string> markdown =
      hover("const LIMIT: i32 := 3\n", 0, 7);
  CHECK(markdown.has_value());
  if (markdown.has_value()) {
    CHECK(markdown->find("const LIMIT: i32") != std::string::npos);
  }
}

TEST_CASE("Hover on a binding name shows the inferred type") {
  const std::optional<std::string> declared =
      hover("fn f() -> i32 {\n  x := 1\n  ret x\n}\n", 1, 2);
  const std::optional<std::string> used =
      hover("fn f() -> i32 {\n  x := 1\n  ret x\n}\n", 2, 6);
  CHECK(declared.has_value());
  CHECK(declared == used);
  if (declared.has_value()) {
    CHECK(declared->find("i32") != std::string::npos);
  }
}

TEST_CASE("Hover keeps the inferred type when borrow checking fails") {
  const std::optional<std::string> markdown = hover(
      "fn main() {\n  x := 1\n  m := &mut x\n  n := &mut x\n"
      "  _ := m\n  _ := n\n}\n",
      1, 2);
  CHECK(markdown.has_value());
  if (markdown.has_value()) {
    CHECK(markdown->find("i32") != std::string::npos);
  }
}

TEST_CASE("Hover on a binding with an annotation shows the written type") {
  const std::optional<std::string> markdown =
      hover("fn f() -> i32 {\n  x: i64 := 1\n  ret 0\n}\n", 1, 2);
  CHECK(markdown.has_value());
  if (markdown.has_value()) {
    CHECK(markdown->find("i64") != std::string::npos);
  }
}

TEST_CASE("Hover on an expression shows its inferred type") {
  const std::optional<std::string> markdown =
      hover("fn f() -> i32 {\n  x := 1\n  ret x\n}\n", 2, 6);
  CHECK(markdown.has_value());
  if (markdown.has_value()) {
    CHECK(markdown->find("i32") != std::string::npos);
  }
}

TEST_CASE("Hover on a type reference resolves the declared type") {
  const std::optional<std::string> markdown = hover(
      "struct Point {\n  x: i32,\n}\nfn f(p: Point) -> i32 {\n  ret 0\n}\n", 3,
      9);
  CHECK(markdown.has_value());
  if (markdown.has_value()) {
    CHECK(markdown->find("Point") != std::string::npos);
    CHECK(markdown->find("size **4**") != std::string::npos);
  }
}

TEST_CASE("Hover on a primitive type shows its spelling") {
  const std::optional<std::string> markdown =
      hover("fn f(p: i32) -> i32 {\n  ret p\n}\n", 0, 9);
  CHECK(markdown.has_value());
  if (markdown.has_value()) {
    CHECK(markdown->find("i32") != std::string::npos);
  }
}

TEST_CASE("Hover on a variadic function shows its variadic slot") {
  const std::optional<std::string> markdown =
      hover("comp fn count(args: ..) -> usize {\n  ret args.len()\n}\n", 0, 10);
  CHECK(markdown.has_value());
  if (markdown.has_value()) {
    CHECK(markdown->find("fn count(args: ..) -> usize") != std::string::npos);
  }
}

TEST_CASE("Hover shows parameter modifiers") {
  const std::optional<std::string> markdown = hover(
      "fn bump(mut x: i32, y: i32) -> i32 {\n  x = y\n  ret x\n}\n", 0, 4);
  CHECK(markdown.has_value());
  if (markdown.has_value()) {
    CHECK(markdown->find("fn bump(mut x: i32, y: i32) -> i32") !=
          std::string::npos);
  }
  const std::optional<std::string> comp =
      hover("fn pick(comp i: usize, x: i32) -> i32 {\n  ret x\n}\n", 0, 3);
  CHECK(comp.has_value());
  if (comp.has_value()) {
    CHECK(comp->find("fn pick(comp i: usize, x: i32) -> i32") !=
          std::string::npos);
  }
}

TEST_CASE("Hover off every node answers nothing") {
  const std::optional<std::string> markdown =
      hover("struct Mix {\n  // just a note\n  a: u8,\n}\n", 1, 6);
  CHECK(!markdown.has_value());
}

}  // namespace lsp
