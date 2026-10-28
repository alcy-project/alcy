// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/code_actions.h"

#include <string>
#include <utility>
#include <vector>

#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "diag/span.h"
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

bool intersects(usize start, usize end, diag::Span span) {
  return span.offset < end && start < span.offset + span.length;
}

}  // namespace

text::JsonValue code_actions(const Document& document,
                             Position start,
                             Position end) {
  const usize start_offset =
      document.index().byte_offset(document.text(), start);
  const usize end_offset = document.index().byte_offset(document.text(), end);

  pipeline::PipelineContext context{i18n::Language::EnUs};
  // The linter's single-source entry is the one that knows a repair, so
  // the action reads the same bag the diagnostics were published from.
  const linter::LintConfig config{};
  const base::Result<linter::LintOutcome, diag::Reported> linted =
      linter::lint_source(context, document.uri(), document.text(), config);
  (void)linted;

  text::JsonValue actions = text::JsonValue::make_array();
  std::vector<std::pair<usize, usize>> seen;
  for (u32 i = 0; i < context.bag.size(); ++i) {
    const diag::Diagnostic* diagnostic = context.bag.at(i);
    if (diagnostic == nullptr || diagnostic->fix == nullptr) {
      continue;
    }
    const diag::Fix& fix = *diagnostic->fix;
    const bool in_range =
        (diagnostic->has_primary_span &&
         intersects(start_offset, end_offset, diagnostic->primary_span)) ||
        intersects(start_offset, end_offset, fix.span);
    if (!in_range) {
      continue;
    }
    bool duplicate = false;
    for (const std::pair<usize, usize>& key : seen) {
      if (key.first == fix.span.offset && key.second == fix.span.length) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) {
      continue;
    }
    seen.emplace_back(fix.span.offset, fix.span.length);

    text::JsonValue edit = text::JsonValue::make_object();
    edit.set("range", range_json(document, fix.span.offset, fix.span.length));
    edit.set("newText",
             text::JsonValue::make_string(std::string(fix.replacement)));
    text::JsonValue edits = text::JsonValue::make_array();
    edits.push(std::move(edit));
    text::JsonValue changes = text::JsonValue::make_object();
    changes.set(std::string(document.uri()), std::move(edits));
    text::JsonValue workspace_edit = text::JsonValue::make_object();
    workspace_edit.set("changes", std::move(changes));

    text::JsonValue action = text::JsonValue::make_object();
    action.set("title",
               text::JsonValue::make_string(std::string(diagnostic->message)));
    action.set("kind", text::JsonValue::make_string("quickfix"));
    action.set("edit", std::move(workspace_edit));
    actions.push(std::move(action));
  }
  return actions;
}

}  // namespace lsp
