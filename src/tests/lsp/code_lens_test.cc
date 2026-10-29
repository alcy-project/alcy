// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/code_lens.h"

#include "doctest/doctest.h"
#include "fpag/base/numeric.h"
#include "lsp/analysis.h"
#include "lsp/document.h"
#include "text/json.h"

namespace lsp {

TEST_CASE("A code lens counts the references of each declaration") {
  const Document document("file:///main.al", "alcy", 1,
                          "fn id(x: i32) -> i32 {\n  ret x\n}\nfn main() -> "
                          "i32 {\n  ret id(1)\n}\n");
  const DocumentAnalysis analysis(document);
  const text::JsonValue lenses = code_lenses(document, analysis);
  CHECK(lenses.as_array().size() == 2);
  const text::JsonValue* lens = nullptr;
  for (const text::JsonValue& candidate : lenses.as_array()) {
    if (candidate.find("range")->find("start")->find("line")->as_number() ==
        0) {
      lens = &candidate;
    }
  }
  CHECK(lens != nullptr);
  if (lens != nullptr) {
    CHECK(lens->find("command")->find("title")->as_string() == "1 reference");
    CHECK(lens->find("command")->find("arguments")->as_array().size() == 3);
  }
}

TEST_CASE("A document that does not analyze has no code lenses") {
  const Document document("file:///broken.al", "alcy", 1,
                          "fn main( -> i32 {\n}\n");
  const DocumentAnalysis analysis(document);
  CHECK(code_lenses(document, analysis).as_array().empty());
}

}  // namespace lsp
