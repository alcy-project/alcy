// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string>
#include <string_view>
#include <unordered_map>

#include "fpag/base/numeric.h"
#include "lsp/document.h"
#include "text/json.h"

namespace lsp {

// The `Diagnostic[]` to publish for one document. The compiler's checker
// runs over the buffer on each call, so the caller sees the diagnostics
// of the text it passed rather than of a cached parse.
text::JsonValue document_diagnostics(const Document& document);

// The pull-model report for one document: a full report whose items are
// the same diagnostics the push model publishes, so a client that pulls
// and one that listens see one answer.
text::JsonValue document_diagnostic_report(const Document& document);

// The pull-model report for every open document: one full report per
// document, each tagged with its URI and version.
text::JsonValue workspace_diagnostic_report(const DocumentStore& documents);

// Remembers the last pull result per document. A client that asks again
// with the id it holds is told the answer is unchanged instead of the
// linter running over an unchanged buffer.
class DiagnosticsCache {
 public:
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
