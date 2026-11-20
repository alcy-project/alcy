// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string>
#include <string_view>
#include <unordered_map>

#include "fpag/base/numeric.h"
#include "lsp/analysis_cache.h"
#include "lsp/document.h"
#include "text/json.h"

namespace lsp {

// The `Diagnostic[]` to publish for one document. Findings come from the
// package's shared checked reading, filtered to this buffer's spans, so
// an import reaching a sibling module is judged against the sibling too.
text::JsonValue document_diagnostics(const Document& document,
                                     AnalysisCache& cache);

// Convenience for a caller without a cache: builds one from the buffer.
text::JsonValue document_diagnostics(const Document& document);

// The pull-model report for one document: a full report whose items are
// the same diagnostics the push model publishes.
text::JsonValue document_diagnostic_report(const Document& document,
                                           AnalysisCache& cache);
text::JsonValue document_diagnostic_report(const Document& document);

// The pull-model report for every open document.
text::JsonValue workspace_diagnostic_report(const DocumentStore& documents,
                                            AnalysisCache& cache);
text::JsonValue workspace_diagnostic_report(const DocumentStore& documents);

// Remembers the last pull result per document. A client that asks again
// with the id it holds is told the answer is unchanged instead of the
// package being analyzed again.
class DiagnosticsCache {
 public:
  text::JsonValue pull(const Document& document,
                       AnalysisCache& cache,
                       std::string_view previous_result_id);
  text::JsonValue pull(const Document& document,
                       std::string_view previous_result_id);
  void erase(std::string_view uri);

 private:
  struct Entry {
    std::string result_id;
    i64 version = -1;
  };

  std::unordered_map<std::string, Entry> entries_;
  u64 next_id_ = 0;
};

}  // namespace lsp
