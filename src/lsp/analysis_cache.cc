// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/analysis_cache.h"

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "fpag/base/numeric.h"
#include "lsp/analysis.h"
#include "lsp/document.h"

namespace lsp {

const DocumentAnalysis& AnalysisCache::get(const Document& document) {
  const auto found = entries_.find(std::string(document.uri()));
  if (found != entries_.end() && found->second.version == document.version()) {
    return *found->second.analysis;
  }
  auto analysis = std::make_unique<DocumentAnalysis>(document);
  Entry entry;
  entry.version = document.version();
  entry.analysis = std::move(analysis);
  const DocumentAnalysis& result = *entry.analysis;
  entries_.insert_or_assign(std::string(document.uri()), std::move(entry));
  return result;
}

void AnalysisCache::erase(std::string_view uri) {
  entries_.erase(std::string(uri));
}

}  // namespace lsp
