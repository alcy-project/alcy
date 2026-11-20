// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/diagnostics.h"

#include <string>
#include <string_view>
#include <utility>

#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "lsp/analysis.h"
#include "lsp/analysis_cache.h"
#include "lsp/document.h"
#include "lsp/protocol.h"
#include "pipeline/pipeline_context.h"
#include "source/source.h"
#include "text/json.h"

namespace lsp {

namespace {

f64 severity_number(diag::Severity severity) {
  switch (severity) {
    case diag::Severity::Error:
      return static_cast<f64>(DiagnosticSeverity::Error);
    case diag::Severity::Warning:
      return static_cast<f64>(DiagnosticSeverity::Warning);
    case diag::Severity::Note:
      return static_cast<f64>(DiagnosticSeverity::Information);
  }
  return static_cast<f64>(DiagnosticSeverity::Error);
}

text::JsonValue diagnostics_for(const Document& document,
                                AnalysisCache& cache) {
  const DocumentAnalysis& analysis = cache.get(document);
  const PackageAnalysis& owner = analysis.owner();
  const source::FileId file = analysis.root();
  const diag::DiagBag& bag = owner.context.bag;

  text::JsonValue diagnostics = text::JsonValue::make_array();
  for (u32 i = 0; i < bag.size(); ++i) {
    const diag::Diagnostic* diagnostic = bag.at(i);
    if (diagnostic == nullptr) {
      continue;
    }
    if (file != source::UNKNOWN_FILE && diagnostic->has_primary_span &&
        diagnostic->primary_span.file != file) {
      continue;
    }
    const u32 offset =
        diagnostic->has_primary_span ? diagnostic->primary_span.offset : 0;
    const u32 length =
        diagnostic->has_primary_span ? diagnostic->primary_span.length : 0;
    text::JsonValue entry = text::JsonValue::make_object();
    entry.set("range", range_json(document, offset, length));
    entry.set("severity", text::JsonValue::make_number(
                              severity_number(diagnostic->severity)));
    entry.set("source", text::JsonValue::make_string("alcy"));
    entry.set("message",
              text::JsonValue::make_string(std::string(diagnostic->message)));
    diagnostics.push(std::move(entry));
  }
  return diagnostics;
}

}  // namespace

text::JsonValue document_diagnostics(const Document& document,
                                     AnalysisCache& cache) {
  return diagnostics_for(document, cache);
}

text::JsonValue document_diagnostics(const Document& document) {
  AnalysisCache cache;
  return diagnostics_for(document, cache);
}

text::JsonValue document_diagnostic_report(const Document& document,
                                           AnalysisCache& cache) {
  text::JsonValue report = text::JsonValue::make_object();
  report.set("kind", text::JsonValue::make_string("full"));
  report.set("items", document_diagnostics(document, cache));
  return report;
}

text::JsonValue document_diagnostic_report(const Document& document) {
  AnalysisCache cache;
  return document_diagnostic_report(document, cache);
}

text::JsonValue workspace_diagnostic_report(const DocumentStore& documents,
                                            AnalysisCache& cache) {
  text::JsonValue items = text::JsonValue::make_array();
  for (const auto& [uri, document] : documents.documents()) {
    (void)uri;
    text::JsonValue report = document_diagnostic_report(document, cache);
    report.set("uri", text::JsonValue::make_string(document.uri()));
    report.set("version", text::JsonValue::make_number(
                              static_cast<f64>(document.version())));
    items.push(std::move(report));
  }
  text::JsonValue out = text::JsonValue::make_object();
  out.set("items", std::move(items));
  return out;
}

text::JsonValue workspace_diagnostic_report(const DocumentStore& documents) {
  AnalysisCache cache(documents);
  return workspace_diagnostic_report(documents, cache);
}

text::JsonValue DiagnosticsCache::pull(const Document& document,
                                       AnalysisCache& cache,
                                       std::string_view previous_result_id) {
  const std::string& uri = document.uri();
  const auto found = entries_.find(uri);
  if (found != entries_.end() && found->second.version == document.version() &&
      !previous_result_id.empty() &&
      found->second.result_id == previous_result_id) {
    text::JsonValue report = text::JsonValue::make_object();
    report.set("kind", text::JsonValue::make_string("unchanged"));
    report.set("resultId",
               text::JsonValue::make_string(found->second.result_id));
    return report;
  }
  const std::string id = std::to_string(next_id_++);
  entries_.insert_or_assign(uri, Entry{id, document.version()});
  text::JsonValue report = document_diagnostic_report(document, cache);
  report.set("resultId", text::JsonValue::make_string(id));
  return report;
}

text::JsonValue DiagnosticsCache::pull(const Document& document,
                                       std::string_view previous_result_id) {
  AnalysisCache cache;
  return pull(document, cache, previous_result_id);
}

void DiagnosticsCache::erase(std::string_view uri) {
  entries_.erase(std::string(uri));
}

}  // namespace lsp
