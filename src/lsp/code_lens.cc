// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/code_lens.h"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "analyzer/types.h"
#include "ast/ast.h"
#include "diag/span.h"
#include "fpag/base/numeric.h"
#include "lsp/analysis.h"
#include "lsp/ast_query.h"
#include "lsp/document.h"
#include "lsp/protocol.h"
#include "pipeline/pipeline_context.h"
#include "source/source.h"
#include "text/json.h"

namespace lsp {

namespace {

u64 span_key(diag::Span span) {
  return (static_cast<u64>(span.offset) << 32) | static_cast<u64>(span.length);
}

std::string reference_title(u32 count) {
  return std::to_string(count) + (count == 1 ? " reference" : " references");
}

text::JsonValue lens_json(const Document& document,
                          diag::Span name,
                          const std::vector<diag::Span>& sites) {
  text::JsonValue lens = text::JsonValue::make_object();
  lens.set("range", range_json(document, name.offset, name.length));

  text::JsonValue locations = text::JsonValue::make_array();
  for (const diag::Span site : sites) {
    locations.push(location_json(document, site));
  }

  text::JsonValue arguments = text::JsonValue::make_array();
  arguments.push(text::JsonValue::make_string(document.uri()));
  arguments.push(position_json(
      document.index().position_at(document.text(), name.offset)));
  arguments.push(std::move(locations));

  text::JsonValue command = text::JsonValue::make_object();
  command.set("title", text::JsonValue::make_string(
                           reference_title(static_cast<u32>(sites.size()))));
  command.set("command",
              text::JsonValue::make_string("editor.action.showReferences"));
  command.set("arguments", std::move(arguments));
  lens.set("command", std::move(command));
  return lens;
}

}  // namespace

text::JsonValue code_lenses(const Document& document,
                            const DocumentAnalysis& analysis) {
  text::JsonValue lenses = text::JsonValue::make_array();
  if (!analysis.ok()) {
    return lenses;
  }
  const pipeline::PipelineContext& context = analysis.context();
  const analyzer::CheckedPackage& package = analysis.package();
  const source::FileId root = analysis.root();

  // One pass over the references groups their sites by the declaration
  // they resolved to, so every lens reads a count without a second walk.
  std::unordered_map<u64, std::vector<diag::Span>> by_declaration;
  for (const analyzer::CheckedModule& module : package.modules) {
    for (const analyzer::CheckedModule::Reference& reference :
         module.references) {
      if (reference.site.file != root || reference.declaration.file != root) {
        continue;
      }
      by_declaration[span_key(reference.declaration)].push_back(reference.site);
    }
  }

  const auto emit = [&](diag::Span name) {
    const auto found = by_declaration.find(span_key(name));
    const std::vector<diag::Span> empty;
    const std::vector<diag::Span>& sites =
        found == by_declaration.end() ? empty : found->second;
    lenses.push(lens_json(document, name, sites));
  };

  std::unordered_set<u32> seen;
  for (const analyzer::CheckedModule& module : package.modules) {
    for (const analyzer::CheckedModule::FnSig& sig : module.functions) {
      if (!sig.item.is_valid() || !seen.insert(sig.item.idx).second) {
        continue;
      }
      const diag::Span name =
          item_name_span(context.ast, context.ast.items[sig.item]);
      if (name.file == root) {
        emit(name);
      }
    }
    const auto emit_struct =
        [&](const analyzer::CheckedModule::StructInfo& info) {
          if (!info.item.is_valid() || !seen.insert(info.item.idx).second) {
            return;
          }
          const diag::Span name =
              item_name_span(context.ast, context.ast.items[info.item]);
          if (name.file == root) {
            emit(name);
          }
        };
    for (const analyzer::CheckedModule::StructInfo& info : module.structs) {
      emit_struct(info);
    }
    for (const analyzer::CheckedModule::EnumInfo& info : module.enums) {
      if (!info.item.is_valid() || !seen.insert(info.item.idx).second) {
        continue;
      }
      const diag::Span name =
          item_name_span(context.ast, context.ast.items[info.item]);
      if (name.file == root) {
        emit(name);
      }
    }
    for (const analyzer::CheckedModule::StaticInfo& info : module.statics) {
      if (!info.item.is_valid() || !seen.insert(info.item.idx).second) {
        continue;
      }
      const diag::Span name =
          item_name_span(context.ast, context.ast.items[info.item]);
      if (name.file == root) {
        emit(name);
      }
    }
  }
  return lenses;
}

}  // namespace lsp
