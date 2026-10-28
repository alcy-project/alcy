// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/analysis_cache.h"

#include "doctest/doctest.h"
#include "lsp/analysis.h"
#include "lsp/document.h"

namespace lsp {

TEST_CASE("A cache reuses the analysis for one version") {
  const Document document("file:///main.al", "alcy", 1,
                          "fn main() -> i32 {\n  ret 0\n}\n");
  AnalysisCache cache;
  const DocumentAnalysis& first = cache.get(document);
  CHECK(first.ok());
  CHECK(&cache.get(document) == &first);
}

TEST_CASE("A cache answers every version of a document") {
  const Document valid("file:///main.al", "alcy", 1,
                       "fn main() -> i32 {\n  ret 0\n}\n");
  const Document broken("file:///main.al", "alcy", 2, "fn main( -> i32 {\n}\n");
  AnalysisCache cache;
  CHECK(cache.get(valid).ok());
  CHECK(!cache.get(broken).ok());
}

TEST_CASE("Erasing an entry leaves the cache able to rebuild it") {
  const Document document("file:///main.al", "alcy", 1,
                          "fn main() -> i32 {\n  ret 0\n}\n");
  AnalysisCache cache;
  CHECK(cache.get(document).ok());
  cache.erase(document.uri());
  CHECK(cache.get(document).ok());
}

}  // namespace lsp
