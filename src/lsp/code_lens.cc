// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/code_lens.h"

#include <optional>
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
#include "lsp/document.h"
#include "lsp/protocol.h"
#include "pipeline/pipeline_context.h"
#include "source/source.h"
#include "text/json.h"

namespace lsp {

namespace {

std::string reference_title(u32 count) {
  return std::to_string(count) + (count == 1 ? " reference" : " references");
}

text::JsonValue lens_json(const Document& document,
                          const DocumentAnalysis& analysis,
                          diag::Span name,
                          const std::vector<diag::Span>& sites) {
  text::JsonValue lens = text::JsonValue::make_object();
  lens.set("range", range_json(document, name.offset, name.length));

  text::JsonValue locations = text::JsonValue::make_array();
  std::unordered_set<FullSpanKey, FullSpanKeyHash> seen;
  for (const diag::Span site : sites) {
    if (!seen.insert({site.file, span_key(site)}).second) {
      continue;
    }
    const std::optional<SourceText> source =
        source_text_of(analysis, site.file);
    if (!source.has_value()) {
      continue;
    }
    locations.push(location_json(source->uri, source->text, site,
                                 analysis.owner().encoding));
  }

  const u32 count = static_cast<u32>(locations.as_array().size());
  text::JsonValue arguments = text::JsonValue::make_array();
  arguments.push(text::JsonValue::make_string(document.uri()));
  arguments.push(position_json(
      document.index().position_at(document.text(), name.offset)));
  arguments.push(std::move(locations));

  text::JsonValue command = text::JsonValue::make_object();
  command.set("title", text::JsonValue::make_string(reference_title(count)));
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
  std::unordered_map<FullSpanKey, std::vector<diag::Span>, FullSpanKeyHash>
      by_declaration;
  for (const analyzer::CheckedModule& module : package.modules) {
    for (const analyzer::CheckedModule::Reference& reference :
         module.references) {
      if (reference.declaration.file != root) {
        continue;
      }
      by_declaration[FullSpanKey{reference.declaration.file,
                                 span_key(reference.declaration)}]
          .push_back(reference.site);
    }
  }

  const auto emit = [&](diag::Span name) {
    const auto found =
        by_declaration.find(FullSpanKey{name.file, span_key(name)});
    const std::vector<diag::Span> empty;
    const std::vector<diag::Span>& sites =
        found == by_declaration.end() ? empty : found->second;
    lenses.push(lens_json(document, analysis, name, sites));
  };

  std::unordered_set<u32> seen;
  for (const analyzer::CheckedModule& module : package.modules) {
    for (const analyzer::CheckedModule::FnSig& sig : module.functions) {
      if (!sig.item.is_valid() || !seen.insert(sig.item.idx).second) {
        continue;
      }
      const diag::Span name =
          ::ast::item_name_span(context.ast, context.ast.items[sig.item]);
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
              ::ast::item_name_span(context.ast, context.ast.items[info.item]);
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
          ::ast::item_name_span(context.ast, context.ast.items[info.item]);
      if (name.file == root) {
        emit(name);
      }
    }
    for (const analyzer::CheckedModule::StaticInfo& info : module.statics) {
      if (!info.item.is_valid() || !seen.insert(info.item.idx).second) {
        continue;
      }
      const diag::Span name =
          ::ast::item_name_span(context.ast, context.ast.items[info.item]);
      if (name.file == root) {
        emit(name);
      }
    }
  }
  return lenses;
}

}  // namespace lsp
