// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/diagnostics.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "diag/stage.h"
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

char severity_code(diag::Severity severity) {
  switch (severity) {
    case diag::Severity::Error: return 'E';
    case diag::Severity::Warning: return 'W';
    case diag::Severity::Note: return 'N';
  }
  return 'E';
}

std::string code_string(diag::Severity severity, diag::Code code) {
  std::string out;
  out.push_back(severity_code(severity));
  out.push_back(stage_letter(code.stage));
  std::string id = std::to_string(code.id);
  while (id.size() < 3) {
    id.insert(id.begin(), '0');
  }
  out += id;
  return out;
}

text::JsonValue diagnostics_for(const Document& document,
                                AnalysisCache& cache) {
  const DocumentAnalysis& analysis = cache.get(document);
  const PackageAnalysis& owner = analysis.owner();
  const source::FileId file = analysis.root();
  const diag::DiagBag& bag = owner.context.bag;
  PositionCache positions;

  text::JsonValue diagnostics = text::JsonValue::make_array();
  for (u32 i = 0; i < bag.size(); ++i) {
    const diag::Diagnostic* diagnostic = bag.at(i);
    if (diagnostic == nullptr) {
      continue;
    }
    // A finding without a primary span has no position in any document;
    // reporting it at offset zero would put it on every open file's
    // first line.
    if (!diagnostic->has_primary_span ||
        (file != source::UNKNOWN_FILE &&
         diagnostic->primary_span.file != file)) {
      continue;
    }
    const u32 offset = diagnostic->primary_span.offset;
    const u32 length = diagnostic->primary_span.length;
    text::JsonValue entry = text::JsonValue::make_object();
    entry.set("range", range_json(document, offset, length));
    entry.set("severity", text::JsonValue::make_number(
                              severity_number(diagnostic->severity)));
    entry.set("source", text::JsonValue::make_string("alcy"));
    if (diagnostic->code.has_value()) {
      entry.set("code", text::JsonValue::make_string(code_string(
                            diagnostic->severity, *diagnostic->code)));
    }
    entry.set("message",
              text::JsonValue::make_string(std::string(diagnostic->message)));
    if (diagnostic->labels != nullptr && diagnostic->label_count != 0) {
      text::JsonValue related = text::JsonValue::make_array();
      for (u32 j = 0; j < diagnostic->label_count; ++j) {
        const diag::Label& label = diagnostic->labels[j];
        const std::optional<std::string> uri =
            uri_of_file(analysis, label.span.file);
        const std::optional<std::string_view> text =
            owner.context.sources.bytes(label.span.file);
        if (!uri.has_value() || !text.has_value()) {
          continue;
        }
        text::JsonValue location = text::JsonValue::make_object();
        location.set("uri", text::JsonValue::make_string(*uri));
        location.set("range",
                     range_json(*text, label.span.offset, label.span.length,
                                owner.encoding, &positions));
        text::JsonValue info = text::JsonValue::make_object();
        info.set("location", std::move(location));
        info.set("message",
                 text::JsonValue::make_string(std::string(label.message)));
        related.push(std::move(info));
      }
      if (!related.as_array().empty()) {
        entry.set("relatedInformation", std::move(related));
      }
    }
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
  const std::string fingerprint = cache.get(document).owner().fingerprint;
  const auto found = entries_.find(uri);
  if (found != entries_.end() && found->second.version == document.version() &&
      found->second.fingerprint == fingerprint && !previous_result_id.empty() &&
      found->second.result_id == previous_result_id) {
    text::JsonValue report = text::JsonValue::make_object();
    report.set("kind", text::JsonValue::make_string("unchanged"));
    report.set("resultId",
               text::JsonValue::make_string(found->second.result_id));
    return report;
  }
  const std::string id = std::to_string(next_id_++);
  entries_.insert_or_assign(uri, Entry{id, fingerprint, document.version()});
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
