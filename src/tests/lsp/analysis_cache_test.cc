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

TEST_CASE("A standalone edit rebuilds even with an unchanged version") {
  const Document before("untitled:Untitled-1", "alcy", 1,
                        "fn main() -> i32 {\n  ret 0\n}\n");
  const Document after("untitled:Untitled-1", "alcy", 1,
                       "fn main() -> i32 {\n  ret 1\n}\n");
  AnalysisCache cache;
  const PackageAnalysis* const first = &cache.get(before).owner();
  cache.invalidate(before);
  CHECK(&cache.get(after).owner() != first);
}

TEST_CASE("An untitled buffer keeps its own standalone signature") {
  const Document first("untitled:Untitled-1", "alcy", 1,
                       "fn main() -> i32 {\n  ret 0\n}\n");
  const Document edited("untitled:Untitled-1", "alcy", 2,
                        "fn main() -> i32 {\n  ret 1\n}\n");
  const Document other("untitled:Untitled-2", "alcy", 1,
                       "fn main() -> i32 {\n  ret 0\n}\n");
  AnalysisCache cache;
  const PackageAnalysis* const before = &cache.get(first).owner();
  CHECK(cache.get(first).ok());
  CHECK(&cache.get(edited).owner() != before);
  CHECK(&cache.get(other).owner() != before);
  CHECK(&cache.get(other).owner() != &cache.get(edited).owner());
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
