// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/selection.h"

#include <span>
#include <vector>

#include "doctest/doctest.h"
#include "fpag/base/numeric.h"
#include "lsp/analysis.h"
#include "lsp/document.h"
#include "text/json.h"

namespace lsp {

namespace {

struct Selected {
  Position start;
  Position end;
};

Position read_position(const text::JsonValue& object) {
  return Position{static_cast<u32>(object.find("line")->as_number()),
                  static_cast<u32>(object.find("character")->as_number())};
}

std::vector<Selected> chain_of(const text::JsonValue& selection) {
  std::vector<Selected> out;
  const text::JsonValue* node = &selection;
  while (node != nullptr && node->is_object()) {
    const text::JsonValue* range = node->find("range");
    if (range == nullptr) {
      break;
    }
    out.push_back(Selected{read_position(*range->find("start")),
                           read_position(*range->find("end"))});
    node = node->find("parent");
  }
  return out;
}

}  // namespace

TEST_CASE("Selection grows from the word to the whole buffer") {
  const Document document("file:///main.al", "alcy", 1,
                          "fn main() -> i32 {\n  x := 1\n  ret x\n}\n");
  const DocumentAnalysis analysis(document);
  const std::vector<Position> positions = {{1, 2}};
  const text::JsonValue result =
      selection_ranges(document, analysis, positions);
  CHECK(result.as_array().size() == 1);
  if (result.as_array().empty()) {
    return;
  }
  const std::vector<Selected> chain = chain_of(result.as_array().front());
  CHECK(chain.size() >= 3);
  if (chain.empty()) {
    return;
  }
  CHECK(chain.front().start.line == 1);
  CHECK(chain.front().start.character == 2);
  CHECK(chain.front().end.character == 3);
  CHECK(chain.back().start.line == 0);
  CHECK(chain.back().start.character == 0);
}

TEST_CASE("One selection range is answered per requested position") {
  const Document document("file:///main.al", "alcy", 1,
                          "fn main() -> i32 {\n  x := 1\n  ret x\n}\n");
  const DocumentAnalysis analysis(document);
  const std::vector<Position> positions = {{1, 2}, {2, 6}};
  const text::JsonValue result =
      selection_ranges(document, analysis, positions);
  CHECK(result.as_array().size() == 2);
}

}  // namespace lsp
