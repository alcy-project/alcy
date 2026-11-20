// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/call_hierarchy.h"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "analyzer/types.h"
#include "ast/ast.h"
#include "diag/span.h"
#include "fpag/base/numeric.h"
#include "lsp/analysis.h"
#include "lsp/document.h"
#include "lsp/navigation.h"
#include "lsp/protocol.h"
#include "pipeline/pipeline_context.h"
#include "source/source.h"
#include "text/json.h"

namespace lsp {

namespace {

bool contains(diag::Span outer, diag::Span inner) {
  return outer.offset <= inner.offset &&
         outer.offset + outer.length >= inner.offset + inner.length;
}

// One callable: the item that declares it and whether it is a method.
struct ItemRef {
  ast::ItemIdx item;
  bool is_method = false;
};

// One call the checker resolved: the callee's name in the call and the
// declaration it resolved to.
struct CallEdge {
  diag::Span site;
  diag::Span declaration;
};

diag::Span name_of(const pipeline::PipelineContext& context,
                   ast::ItemIdx item) {
  return ::ast::item_name_span(context.ast, context.ast.items[item]);
}

// The callable a declaration span names, when the checker registered it
// as a function or a method.
std::optional<ItemRef> item_of_declaration(
    const pipeline::PipelineContext& context,
    const analyzer::CheckedPackage& package,
    diag::Span declaration) {
  for (const analyzer::CheckedModule& module : package.modules) {
    for (const analyzer::CheckedModule::MethodInfo& info : module.methods) {
      if (info.item.is_valid() &&
          same_span(name_of(context, info.item), declaration)) {
        return ItemRef{info.item, true};
      }
    }
    for (const analyzer::CheckedModule::FnSig& sig : module.functions) {
      if (sig.item.is_valid() &&
          same_span(name_of(context, sig.item), declaration)) {
        return ItemRef{sig.item, sig.is_method};
      }
    }
  }
  return std::nullopt;
}

// The smallest function item whose span holds the offset: the caller a
// call site sits in.
std::optional<ItemRef> enclosing_function(
    const pipeline::PipelineContext& context,
    const analyzer::CheckedPackage& package,
    usize offset,
    source::FileId site_file) {
  std::optional<ItemRef> best;
  usize best_length = 0;
  for (const analyzer::CheckedModule& module : package.modules) {
    for (const analyzer::CheckedModule::FnSig& sig : module.functions) {
      if (!sig.item.is_valid()) {
        continue;
      }
      const ast::ItemNode& node = context.ast.items[sig.item];
      if (node.kind != ast::ItemKind::Fn || node.span.file != site_file) {
        continue;
      }
      if (node.span.offset <= offset &&
          offset < node.span.offset + node.span.length &&
          (!best.has_value() || node.span.length < best_length)) {
        best = ItemRef{sig.item, sig.is_method};
        best_length = node.span.length;
      }
    }
  }
  return best;
}

// Every call the checker recorded in the root file, deduplicated. A
// call site is recognized by span identity: the callee expression's span
// for a call, the member name's span for a method call, matched against
// the reference table's site. The parser and the checker agree on those
// spans, so the join is exact.
std::vector<CallEdge> call_edges(const pipeline::PipelineContext& context,
                                 const analyzer::CheckedPackage& package,
                                 source::FileId root) {
  std::unordered_set<FullSpanKey, FullSpanKeyHash> sites;
  context.ast.exprs.for_each_node([&](const ast::ExprIdx index) {
    const ast::ExprNode& node = context.ast.exprs[index];
    if (root != source::UNKNOWN_FILE && node.span.file != root) {
      return;
    }
    if (node.kind == ast::ExprKind::Call) {
      const ast::ExprIdx callee = node.payload.get<ast::ExprCall>().callee;
      const diag::Span span = context.ast.exprs[callee].span;
      sites.insert(FullSpanKey{span.file, span_key(span)});
    } else if (node.kind == ast::ExprKind::MethodCall) {
      const diag::Span span = node.payload.get<ast::ExprMethodCall>().name.span;
      sites.insert(FullSpanKey{span.file, span_key(span)});
    }
  });

  struct EdgeKey {
    FullSpanKey site;
    FullSpanKey declaration;
    bool operator==(const EdgeKey&) const = default;
  };
  struct EdgeKeyHash {
    usize operator()(const EdgeKey& key) const {
      const FullSpanKeyHash hash;
      return hash(key.site) ^ (hash(key.declaration) << 1);
    }
  };
  std::vector<CallEdge> edges;
  std::unordered_set<EdgeKey, EdgeKeyHash> seen;
  for (const analyzer::CheckedModule& module : package.modules) {
    for (const analyzer::CheckedModule::Reference& reference :
         module.references) {
      const FullSpanKey site{reference.site.file, span_key(reference.site)};
      if (!sites.contains(site)) {
        continue;
      }
      const FullSpanKey declaration{reference.declaration.file,
                                    span_key(reference.declaration)};
      if (!seen.insert(EdgeKey{site, declaration}).second) {
        continue;
      }
      edges.push_back(CallEdge{reference.site, reference.declaration});
    }
  }
  return edges;
}

diag::Span span_of(const text::JsonValue& data,
                   std::string_view offset_key,
                   std::string_view length_key,
                   source::FileId root) {
  diag::Span span;
  span.file = root;
  const text::JsonValue* offset = data.find(offset_key);
  const text::JsonValue* length = data.find(length_key);
  if (offset != nullptr) {
    span.offset = static_cast<u32>(offset->as_number());
  }
  if (length != nullptr) {
    span.length = static_cast<u32>(length->as_number());
  }
  return span;
}

text::JsonValue item_json(std::string_view uri,
                          std::string_view text,
                          const pipeline::PipelineContext& context,
                          ItemRef ref) {
  const ast::ItemNode& node = context.ast.items[ref.item];
  const diag::Span name = name_of(context, ref.item);
  text::JsonValue item = text::JsonValue::make_object();
  item.set("name", text::JsonValue::make_string(std::string(node.name())));
  item.set("kind",
           text::JsonValue::make_number(static_cast<f64>(
               ref.is_method ? SymbolKind::Method : SymbolKind::Function)));
  item.set("uri", text::JsonValue::make_string(std::string(uri)));
  item.set("range", range_json(text, node.span.offset, node.span.length));
  item.set("selectionRange", range_json(text, name.offset, name.length));
  text::JsonValue data = text::JsonValue::make_object();
  data.set("nameOffset", text::JsonValue::make_number(name.offset));
  data.set("nameLength", text::JsonValue::make_number(name.length));
  data.set("rangeOffset", text::JsonValue::make_number(node.span.offset));
  data.set("rangeLength", text::JsonValue::make_number(node.span.length));
  item.set("data", std::move(data));
  return item;
}

// The URI and bytes of a file the analysis registered, when it has both.
bool source_text(const DocumentAnalysis& analysis,
                 source::FileId file,
                 std::string& uri,
                 std::string_view& text) {
  const std::optional<std::string> found = uri_of_file(analysis, file);
  if (!found.has_value()) {
    return false;
  }
  const std::optional<std::string_view> bytes =
      analysis.context().sources.bytes(file);
  if (!bytes.has_value()) {
    return false;
  }
  uri = *found;
  text = *bytes;
  return true;
}

const text::JsonValue* item_data(const text::JsonValue& item) {
  const text::JsonValue* data = item.find("data");
  if (data == nullptr || !data->is_object()) {
    return nullptr;
  }
  return data;
}

}  // namespace

text::JsonValue prepare_call_hierarchy(const Document& document,
                                       const DocumentAnalysis& analysis,
                                       Position position) {
  text::JsonValue items = text::JsonValue::make_array();
  if (!analysis.ok()) {
    return items;
  }
  const std::optional<diag::Span> declaration =
      definition_at(document, analysis, position);
  if (!declaration.has_value()) {
    return items;
  }
  const std::optional<ItemRef> ref =
      item_of_declaration(analysis.context(), analysis.package(), *declaration);
  if (!ref.has_value()) {
    return items;
  }
  std::string uri;
  std::string_view text;
  if (!source_text(analysis, declaration->file, uri, text)) {
    return items;
  }
  items.push(item_json(uri, text, analysis.context(), *ref));
  return items;
}

text::JsonValue incoming_calls(const Document& document,
                               const DocumentAnalysis& analysis,
                               const text::JsonValue& item) {
  (void)document;
  text::JsonValue calls = text::JsonValue::make_array();
  const text::JsonValue* data = item_data(item);
  if (!analysis.ok() || data == nullptr) {
    return calls;
  }
  const diag::Span target =
      span_of(*data, "nameOffset", "nameLength", analysis.root());
  const pipeline::PipelineContext& context = analysis.context();

  struct Caller {
    ItemRef ref;
    source::FileId file = source::UNKNOWN_FILE;
  };
  std::vector<Caller> callers;
  std::vector<std::vector<diag::Span>> ranges;
  for (const CallEdge& edge :
       call_edges(context, analysis.package(), source::UNKNOWN_FILE)) {
    if (!same_span(edge.declaration, target)) {
      continue;
    }
    const std::optional<ItemRef> caller = enclosing_function(
        context, analysis.package(), edge.site.offset, edge.site.file);
    if (!caller.has_value()) {
      continue;
    }
    usize at = callers.size();
    for (usize i = 0; i < callers.size(); ++i) {
      if (callers[i].ref.item == caller->item &&
          callers[i].file == edge.site.file) {
        at = i;
        break;
      }
    }
    if (at == callers.size()) {
      callers.push_back(Caller{*caller, edge.site.file});
      ranges.emplace_back();
    }
    ranges[at].push_back(edge.site);
  }

  for (usize i = 0; i < callers.size(); ++i) {
    std::string uri;
    std::string_view text;
    if (!source_text(analysis, callers[i].file, uri, text)) {
      continue;
    }
    text::JsonValue call = text::JsonValue::make_object();
    call.set("from", item_json(uri, text, context, callers[i].ref));
    text::JsonValue from_ranges = text::JsonValue::make_array();
    for (const diag::Span range : ranges[i]) {
      from_ranges.push(range_json(text, range.offset, range.length));
    }
    call.set("fromRanges", std::move(from_ranges));
    calls.push(std::move(call));
  }
  return calls;
}

text::JsonValue outgoing_calls(const Document& document,
                               const DocumentAnalysis& analysis,
                               const text::JsonValue& item) {
  text::JsonValue calls = text::JsonValue::make_array();
  const text::JsonValue* data = item_data(item);
  if (!analysis.ok() || data == nullptr) {
    return calls;
  }
  const diag::Span body =
      span_of(*data, "rangeOffset", "rangeLength", analysis.root());
  const pipeline::PipelineContext& context = analysis.context();

  struct Callee {
    ItemRef ref;
    source::FileId file = source::UNKNOWN_FILE;
  };
  std::vector<Callee> callees;
  std::vector<std::vector<diag::Span>> ranges;
  for (const CallEdge& edge :
       call_edges(context, analysis.package(), analysis.root())) {
    if (!contains(body, edge.site)) {
      continue;
    }
    const std::optional<ItemRef> callee =
        item_of_declaration(context, analysis.package(), edge.declaration);
    if (!callee.has_value()) {
      continue;
    }
    usize at = callees.size();
    for (usize i = 0; i < callees.size(); ++i) {
      if (callees[i].ref.item == callee->item &&
          callees[i].file == edge.declaration.file) {
        at = i;
        break;
      }
    }
    if (at == callees.size()) {
      callees.push_back(Callee{*callee, edge.declaration.file});
      ranges.emplace_back();
    }
    ranges[at].push_back(edge.site);
  }

  for (usize i = 0; i < callees.size(); ++i) {
    std::string uri;
    std::string_view text;
    if (!source_text(analysis, callees[i].file, uri, text)) {
      continue;
    }
    text::JsonValue call = text::JsonValue::make_object();
    call.set("to", item_json(uri, text, context, callees[i].ref));
    text::JsonValue from_ranges = text::JsonValue::make_array();
    for (const diag::Span range : ranges[i]) {
      from_ranges.push(range_json(document, range.offset, range.length));
    }
    call.set("fromRanges", std::move(from_ranges));
    calls.push(std::move(call));
  }
  return calls;
}

}  // namespace lsp
