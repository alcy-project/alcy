// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/formatting.h"

#include <string>
#include <string_view>
#include <utility>

#include "fmt/formatter.h"
#include "lsp/document.h"
#include "lsp/protocol.h"
#include "text/json.h"

namespace lsp {

text::JsonValue document_formatting(const Document& document) {
  text::JsonValue edits = text::JsonValue::make_array();
  std::string formatted =
      alcy_fmt::format(document.text(), alcy_fmt::default_format_config());
  if (std::string_view(formatted) == document.text()) {
    return edits;
  }
  const Position end =
      document.index().position_at(document.text(), document.text().size());

  text::JsonValue range = text::JsonValue::make_object();
  range.set("start", position_json(Position{0, 0}));
  range.set("end", position_json(end));

  text::JsonValue edit = text::JsonValue::make_object();
  edit.set("range", std::move(range));
  edit.set("newText", text::JsonValue::make_string(std::move(formatted)));
  edits.push(std::move(edit));
  return edits;
}

text::JsonValue document_range_formatting(const Document& document,
                                          Position start,
                                          Position end) {
  const std::string_view original = document.text();
  const std::string formatted =
      alcy_fmt::format(original, alcy_fmt::default_format_config());
  text::JsonValue edits = text::JsonValue::make_array();
  if (formatted == original) {
    return edits;
  }
  usize prefix = 0;
  while (prefix < original.size() && prefix < formatted.size() &&
         original[prefix] == formatted[prefix]) {
    ++prefix;
  }
  usize suffix = 0;
  while (suffix < original.size() - prefix &&
         suffix < formatted.size() - prefix &&
         original[original.size() - 1 - suffix] ==
             formatted[formatted.size() - 1 - suffix]) {
    ++suffix;
  }
  const usize changed_start = prefix;
  const usize changed_end = original.size() - suffix;
  const usize asked_start = document.index().byte_offset(original, start);
  const usize asked_end = document.index().byte_offset(original, end);
  if (changed_end <= asked_start || changed_start >= asked_end) {
    return edits;
  }
  text::JsonValue range = text::JsonValue::make_object();
  range.set("start", position_json(document.index().position_at(
                         original, changed_start)));
  range.set("end",
            position_json(document.index().position_at(original, changed_end)));
  text::JsonValue edit = text::JsonValue::make_object();
  edit.set("range", std::move(range));
  edit.set("newText", text::JsonValue::make_string(formatted.substr(
                          prefix, formatted.size() - prefix - suffix)));
  edits.push(std::move(edit));
  return edits;
}

}  // namespace lsp
