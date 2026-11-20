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

TEST_CASE("Range formatting leaves canonical text alone") {
  const Document document("file:///main.al", "alcy", 1,
                          "struct Point {\n  x: i32,\n}\n");
  const text::JsonValue edits =
      document_range_formatting(document, Position{0, 0}, Position{2, 1});
  CHECK(edits.as_array().empty());
}

}  // namespace lsp
