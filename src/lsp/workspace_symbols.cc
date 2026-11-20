// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/workspace_symbols.h"

#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "analyzer/resolve.h"
#include "fpag/base/numeric.h"
#include "lsp/analysis.h"
#include "lsp/analysis_cache.h"
#include "lsp/document.h"
#include "lsp/symbols.h"
#include "source/source.h"
#include "text/json.h"

namespace lsp {

namespace {

char lower(char c) {
  return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

// A fuzzy match: the query's characters appear in the name in order,
// which is the filtering a quick-open box expects. A substring is one
// such match, so the answer stays a superset of an exact search.
bool matches(std::string_view name, std::string_view query) {
  if (query.empty()) {
    return true;
  }
  usize at = 0;
  for (const char c : name) {
    if (at < query.size() && lower(c) == lower(query[at])) {
      ++at;
    }
  }
  return at == query.size();
}

std::string child_container(std::string_view container, std::string_view name) {
  if (container.empty()) {
    return std::string(name);
  }
  std::string out(container);
  out += '.';
  out += name;
  return out;
}

void collect(std::string_view uri,
             const text::JsonValue& symbols,
             std::string_view container,
             std::string_view query,
             text::JsonValue* out) {
  for (const text::JsonValue& symbol : symbols.as_array()) {
    const text::JsonValue* name_value = symbol.find("name");
    const text::JsonValue* kind_value = symbol.find("kind");
    const text::JsonValue* range = symbol.find("range");
    if (name_value == nullptr || kind_value == nullptr || range == nullptr) {
      continue;
    }
    const std::string_view name = name_value->as_string();
    if (matches(name, query)) {
      text::JsonValue info = text::JsonValue::make_object();
      info.set("name", text::JsonValue::make_string(std::string(name)));
      info.set("kind", text::JsonValue::make_number(kind_value->as_number()));
      if (!container.empty()) {
        info.set("containerName",
                 text::JsonValue::make_string(std::string(container)));
      }
      text::JsonValue location = text::JsonValue::make_object();
      location.set("uri", text::JsonValue::make_string(std::string(uri)));
      location.set("range", *range);
      info.set("location", std::move(location));
      out->push(std::move(info));
    }
    const text::JsonValue* children = symbol.find("children");
    if (children != nullptr && children->is_array() &&
        !children->as_array().empty()) {
      const std::string nested = child_container(container, name);
      collect(uri, *children, nested, query, out);
    }
  }
}

}  // namespace

text::JsonValue workspace_symbols(const DocumentStore& documents,
                                  AnalysisCache& cache,
                                  std::string_view query) {
  std::map<std::string, std::string> sources;
  PositionEncoding encoding = PositionEncoding::Utf16;
  for (const auto& [uri, document] : documents.documents()) {
    (void)uri;
    const DocumentAnalysis& analysis = cache.get(document);
    encoding = analysis.owner().encoding;
    if (!analysis.ok()) {
      continue;
    }
    for (const analyzer::ModuleNode* module : analysis.package().tree.modules) {
      if (module->file == source::UNKNOWN_FILE) {
        continue;
      }
      const std::optional<std::string> file_uri =
          uri_of_file(analysis, module->file);
      const std::optional<std::string_view> text =
          analysis.context().sources.bytes(module->file);
      if (file_uri.has_value() && text.has_value()) {
        sources.insert_or_assign(*file_uri, std::string(*text));
      }
    }
  }
  text::JsonValue out = text::JsonValue::make_array();
  for (const auto& [uri, text] : sources) {
    Document document(uri, "alcy", 1, text);
    document.set_encoding(encoding);
    const text::JsonValue symbols = document_symbols(document);
    collect(uri, symbols, {}, query, &out);
  }
  return out;
}

text::JsonValue workspace_symbols(const DocumentStore& documents,
                                  std::string_view query) {
  AnalysisCache cache(documents);
  return workspace_symbols(documents, cache, query);
}

}  // namespace lsp
