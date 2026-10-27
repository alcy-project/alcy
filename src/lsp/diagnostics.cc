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
#include "i18n/language.h"
#include "linter/config.h"
#include "linter/lint.h"
#include "lsp/document.h"
#include "pipeline/pipeline_context.h"
#include "text/json.h"

namespace lsp {

namespace {

text::JsonValue position_json(Position position) {
  text::JsonValue out = text::JsonValue::make_object();
  out.set("line", text::JsonValue::make_number(position.line));
  out.set("character", text::JsonValue::make_number(position.character));
  return out;
}

text::JsonValue range_json(const Document& document, u32 offset, u32 length) {
  text::JsonValue out = text::JsonValue::make_object();
  out.set("start",
          position_json(document.index().position_at(document.text(), offset)));
  out.set("end", position_json(document.index().position_at(
                     document.text(), static_cast<usize>(offset) + length)));
  return out;
}

// LSP DiagnosticSeverity: error, warning, information, hint. The
// compiler's notes are information; it has no hint.
f64 severity_number(diag::Severity severity) {
  switch (severity) {
    case diag::Severity::Error: return 1;
    case diag::Severity::Warning: return 2;
    case diag::Severity::Note: return 3;
  }
  return 1;
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

}  // namespace lsp
