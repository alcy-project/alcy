// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/workspace_symbols.h"

#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "fpag/base/numeric.h"
#include "lsp/document.h"
#include "lsp/symbols.h"
#include "text/json.h"

namespace lsp {

namespace {

char lower(char c) {
  return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool matches(std::string_view name, std::string_view query) {
  if (query.empty()) {
    return true;
  }
  if (query.size() > name.size()) {
    return false;
  }
  for (usize i = 0; i + query.size() <= name.size(); ++i) {
    bool same = true;
    for (usize j = 0; j < query.size(); ++j) {
      if (lower(name[i + j]) != lower(query[j])) {
        same = false;
        break;
      }
    }
    if (same) {
      return true;
    }
  }
  return false;
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

void collect(const Document& document,
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
      location.set("uri", text::JsonValue::make_string(document.uri()));
      location.set("range", *range);
      info.set("location", std::move(location));
      out->push(std::move(info));
    }
    const text::JsonValue* children = symbol.find("children");
    if (children != nullptr && children->is_array() &&
        !children->as_array().empty()) {
      const std::string nested = child_container(container, name);
      collect(document, *children, nested, query, out);
    }
  }
}

}  // namespace

text::JsonValue workspace_symbols(const DocumentStore& documents,
                                  std::string_view query) {
  text::JsonValue out = text::JsonValue::make_array();
  for (const auto& entry : documents.documents()) {
    const text::JsonValue symbols = document_symbols(entry.second);
    collect(entry.second, symbols, {}, query, &out);
  }
  return out;
}

}  // namespace lsp
