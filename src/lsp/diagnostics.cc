// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/diagnostics.h"

#include <string>
#include <utility>

#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "i18n/language.h"
#include "linter/config.h"
#include "linter/lint.h"
#include "lsp/document.h"
#include "lsp/protocol.h"
#include "pipeline/pipeline_context.h"
#include "text/json.h"

namespace lsp {

namespace {

// LSP DiagnosticSeverity: error, warning, information, hint. The
// compiler's notes are information; it has no hint.
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

}  // namespace

text::JsonValue document_diagnostics(const Document& document) {
  pipeline::PipelineContext context{i18n::Language::EnUs};
  // The linter's single-source entry runs the frontend and the rules, so
  // one pass publishes both the compiler's errors and the lint findings
  // rather than checking the buffer twice.
  const linter::LintConfig config{};
  const base::Result<linter::LintOutcome, diag::Reported> result =
      linter::lint_source(context, document.uri(), document.text(), config);
  (void)result;

  text::JsonValue diagnostics = text::JsonValue::make_array();
  for (u32 i = 0; i < context.bag.size(); ++i) {
    const diag::Diagnostic* diagnostic = context.bag.at(i);
    if (diagnostic == nullptr) {
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

text::JsonValue document_diagnostic_report(const Document& document) {
  text::JsonValue report = text::JsonValue::make_object();
  report.set("kind", text::JsonValue::make_string("full"));
  report.set("items", document_diagnostics(document));
  return report;
}

}  // namespace lsp
