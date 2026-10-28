// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

#include "fpag/base/numeric.h"
#include "lsp/analysis.h"
#include "lsp/document.h"

namespace lsp {

// One checked reading per open buffer, keyed by URI and invalidated by
// version. A request for a buffer that did not change reuses the package
// the previous request built, so the frontend runs once per edit rather
// than once per request.
class AnalysisCache {
 public:
  // The analysis of the document at its current version, built on the
  // first request. The reference stays valid until the next request for
  // the same URI or an erase; check ok() before reading the package.
  const DocumentAnalysis& get(const Document& document);

  // Drops the entry for a closed buffer.
  void erase(std::string_view uri);

 private:
  struct Entry {
    u32 version = 0;
    std::unique_ptr<DocumentAnalysis> analysis;
  };
  std::unordered_map<std::string, Entry> entries_;
};

}  // namespace lsp
