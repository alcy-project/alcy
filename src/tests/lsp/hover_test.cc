// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/hover.h"

#include <optional>
#include <string>

#include "doctest/doctest.h"
#include "lsp/document.h"

namespace lsp {

namespace {

std::optional<std::string> hover(const std::string& text,
                                 u32 line,
                                 u32 character) {
  const Document document("file:///main.al", "alcy", 1, text);
  return hover_markdown(document, Position{line, character});
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
    CHECK(markdown->find("fn add(i32, i32) -> i32") != std::string::npos);
  }
}

TEST_CASE("Hover on a const shows its decided type") {
  const std::optional<std::string> markdown =
      hover("const LIMIT: i32 = 3\n", 0, 7);
  CHECK(markdown.has_value());
  if (markdown.has_value()) {
    CHECK(markdown->find("const LIMIT: i32") != std::string::npos);
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
    CHECK(markdown->find("size 4") != std::string::npos);
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

TEST_CASE("Hover off every node answers nothing") {
  const std::optional<std::string> markdown =
      hover("struct Mix {\n  // just a note\n  a: u8,\n}\n", 1, 6);
  CHECK(!markdown.has_value());
}

}  // namespace lsp
