// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "ssg/highlight.h"

#include <algorithm>
#include <fstream>
#include <ios>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ssg/markdown.h"
#include "tree_sitter/api.h"

extern "C" const TSLanguage* tree_sitter_alcy();

namespace ssg {

struct Highlight::Impl {
  TSParser* parser = nullptr;
  TSQuery* query = nullptr;

  ~Impl() {
    if (query != nullptr) {
      ts_query_delete(query);
    }
    if (parser != nullptr) {
      ts_parser_delete(parser);
    }
  }
};

namespace {

// Same rendering rules as the shared TypeScript highlighter: a
// capture maps to one CSS class per ancestor, so a theme styles
// `keyword` once and overrides `keyword.control` where it wants
// to. Captures overwrite each other by priority rather than by
// query order.

constexpr i32 priority_of(std::string_view name) {
  const usize dot = name.find('.');
  const std::string_view root = name.substr(0, dot);
  if (root == "comment" || root == "constant" || root == "keyword" ||
      root == "label" || root == "number" || root == "string") {
    return 3;
  }
  if (root == "operator" || root == "punctuation") {
    return 2;
  }
  if (root == "variable") {
    return 1;
  }
  if (root == "constructor" || root == "function" || root == "namespace" ||
      root == "property" || root == "type") {
    return 4;
  }
  return 2;
}

std::string classes_for(std::string_view name) {
  std::string out;
  usize begin = 0;
  bool first = true;
  std::string prefix;
  while (begin <= name.size()) {
    const usize dot = name.find('.', begin);
    const usize end = dot == std::string_view::npos ? name.size() : dot;
    if (!prefix.empty()) {
      prefix += '-';
    }
    prefix += name.substr(begin, end - begin);
    if (!first) {
      out += ' ';
    }
    first = false;
    out += "hl-";
    out += prefix;
    if (dot == std::string_view::npos) {
      break;
    }
    begin = dot + 1;
  }
  return out;
}

struct Capture {
  const char* name = nullptr;
  u32 start = 0;
  u32 end = 0;
};

std::string render(std::string_view text, std::vector<Capture> captures) {
  std::stable_sort(captures.begin(), captures.end(),
                   [](const Capture& a, const Capture& b) {
                     return priority_of(a.name) < priority_of(b.name);
                   });
  std::vector<i32> classes(text.size(), 0);
  std::unordered_map<std::string, i32> capture_ids;
  std::vector<std::string> capture_classes;
  for (const Capture& capture : captures) {
    const auto existing = capture_ids.find(capture.name);
    i32 id = 0;
    if (existing == capture_ids.end()) {
      capture_classes.push_back(classes_for(capture.name));
      id = static_cast<i32>(capture_classes.size());
      capture_ids.emplace(capture.name, id);
    } else {
      id = existing->second;
    }
    u32 start = capture.start;
    u32 end = capture.end;
    end = std::max(start, std::min(static_cast<u32>(text.size()), end));
    if (start >= end) {
      continue;
    }
    std::fill(classes.begin() + start, classes.begin() + end, id);
  }
  std::string out;
  out.reserve(text.size() + 64);
  usize index = 0;
  while (index < text.size()) {
    const i32 id = classes[index];
    usize rend = index + 1;
    while (rend < text.size() && classes[rend] == id) {
      rend++;
    }
    const std::string chunk = escape_html(text.substr(index, rend - index));
    if (id == 0) {
      out += chunk;
    } else {
      out += "<span class=\"";
      out += capture_classes[id - 1];
      out += "\">";
      out += chunk;
      out += "</span>";
    }
    index = rend;
  }
  return out;
}
}  // namespace

base::Result<Highlight, std::string> create_highlighter(
    const std::string& highlights_path) {
  std::ifstream input(highlights_path, std::ios::binary);
  if (!input) {
    return base::make_err("cannot read the highlights query: " +
                          highlights_path);
  }
  std::ostringstream bytes;
  bytes << input.rdbuf();
  const std::string source = bytes.str();

  Highlight highlighter;
  auto impl = std::make_unique<Highlight::Impl>();
  const TSLanguage* language = tree_sitter_alcy();
  TSParser* parser = ts_parser_new();
  if (parser == nullptr) {
    return base::make_err(
        std::string("the tree-sitter parser could not be created"));
  }
  ts_parser_set_language(parser, language);
  impl->parser = parser;
  u32 error_offset = 0;
  TSQueryError error_type = TSQueryErrorNone;
  impl->query =
      ts_query_new(language, source.c_str(), static_cast<u32>(source.size()),
                   &error_offset, &error_type);
  if (impl->query == nullptr) {
    return base::make_err("the highlights query did not parse at offset " +
                          std::to_string(error_offset));
  }
  highlighter.impl_ = std::move(impl);
  return base::make_ok(std::move(highlighter));
}

std::string Highlight::highlight_code(std::string_view code) const {
  if (impl_ == nullptr) {
    return escape_html(code);
  }
  TSTree* tree = ts_parser_parse_string(impl_->parser, nullptr, code.data(),
                                        static_cast<u32>(code.size()));
  if (tree == nullptr) {
    return escape_html(code);
  }
  TSNode root = ts_tree_root_node(tree);
  TSQueryCursor* cursor = ts_query_cursor_new();
  ts_query_cursor_exec(cursor, impl_->query, root);
  std::vector<Capture> captures;
  TSQueryMatch match;
  u32 capture_index = 0;
  while (ts_query_cursor_next_capture(cursor, &match, &capture_index)) {
    const TSQueryCapture capture = match.captures[capture_index];
    u32 name_length = 0;
    const char* name =
        ts_query_capture_name_for_id(impl_->query, capture.index, &name_length);
    captures.push_back({.name = name,
                        .start = ts_node_start_byte(capture.node),
                        .end = ts_node_end_byte(capture.node)});
  }
  ts_query_cursor_delete(cursor);
  ts_tree_delete(tree);
  return render(code, std::move(captures));
}

Highlight::Highlight() = default;
Highlight::~Highlight() = default;
Highlight::Highlight(Highlight&& other) noexcept = default;
Highlight& Highlight::operator=(Highlight&& other) noexcept = default;

}  // namespace ssg
