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
  const DocumentAnalysis& first = cache.analysis_for(document);
  CHECK(first.ok());
  CHECK(&cache.analysis_for(document) == &first);
}

TEST_CASE("A cache answers every version of a document") {
  const Document valid("file:///main.al", "alcy", 1,
                       "fn main() -> i32 {\n  ret 0\n}\n");
  const Document broken("file:///main.al", "alcy", 2, "fn main( -> i32 {\n}\n");
  AnalysisCache cache;
  CHECK(cache.analysis_for(valid).ok());
  CHECK(!cache.analysis_for(broken).ok());
}

TEST_CASE("A standalone edit rebuilds even with an unchanged version") {
  const Document before("untitled:Untitled-1", "alcy", 1,
                        "fn main() -> i32 {\n  ret 0\n}\n");
  const Document after("untitled:Untitled-1", "alcy", 1,
                       "fn main( -> i32 {\n}\n");
  AnalysisCache cache;
  CHECK(cache.analysis_for(before).ok());
  cache.invalidate(before);
  CHECK(!cache.analysis_for(after).ok());
}

TEST_CASE("An untitled buffer keeps its own standalone signature") {
  const Document first("untitled:Untitled-1", "alcy", 1,
                       "fn main() -> i32 {\n  ret 0\n}\n");
  const Document edited("untitled:Untitled-1", "alcy", 2,
                        "fn main( -> i32 {\n}\n");
  const Document other("untitled:Untitled-2", "alcy", 1,
                       "fn main() -> i32 {\n  ret 0\n}\n");
  AnalysisCache cache;
  CHECK(cache.analysis_for(first).ok());
  CHECK(!cache.analysis_for(edited).ok());
  CHECK(cache.analysis_for(other).ok());
}

TEST_CASE("Erasing an entry leaves the cache able to rebuild it") {
  const Document document("file:///main.al", "alcy", 1,
                          "fn main() -> i32 {\n  ret 0\n}\n");
  AnalysisCache cache;
  CHECK(cache.analysis_for(document).ok());
  cache.erase(document.uri());
  CHECK(cache.analysis_for(document).ok());
}

}  // namespace lsp
