// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

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

}  // namespace lsp
