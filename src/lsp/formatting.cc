// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/formatting.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

struct Line {
  std::string_view text;
  usize start = 0;
};

std::vector<Line> split_lines(std::string_view text) {
  std::vector<Line> lines;
  usize start = 0;
  for (usize i = 0; i < text.size(); ++i) {
    if (text[i] == '\n') {
      lines.push_back(Line{text.substr(start, i + 1 - start), start});
      start = i + 1;
    }
  }
  if (start < text.size()) {
    lines.push_back(Line{text.substr(start), start});
  }
  return lines;
}

std::string join_lines(const std::vector<Line>& lines, usize from, usize to) {
  std::string out;
  for (usize i = from; i < to; ++i) {
    out.append(lines[i].text);
  }
  return out;
}

void append_edit(const Document& document,
                 std::string_view full_text,
                 std::string_view hunk,
                 std::string_view replacement,
                 usize start,
                 usize end,
                 usize asked_start,
                 usize asked_end,
                 text::JsonValue* edits) {
  if (end <= asked_start || start >= asked_end) {
    return;
  }
  usize prefix = 0;
  while (prefix < hunk.size() && prefix < replacement.size() &&
         hunk[prefix] == replacement[prefix]) {
    ++prefix;
  }
  usize suffix = 0;
  while (suffix < hunk.size() - prefix &&
         suffix < replacement.size() - prefix &&
         hunk[hunk.size() - 1 - suffix] ==
             replacement[replacement.size() - 1 - suffix]) {
    ++suffix;
  }
  const usize from = start + prefix;
  const usize to = end - suffix;
  const std::string_view inserted =
      replacement.substr(prefix, replacement.size() - prefix - suffix);
  if (from > to || (from == to && inserted.empty())) {
    return;
  }
  if (to <= asked_start || from >= asked_end) {
    return;
  }
  text::JsonValue range = text::JsonValue::make_object();
  range.set("start",
            position_json(document.index().position_at(full_text, from)));
  range.set("end", position_json(document.index().position_at(full_text, to)));
  text::JsonValue edit = text::JsonValue::make_object();
  edit.set("range", std::move(range));
  edit.set("newText", text::JsonValue::make_string(std::string(inserted)));
  edits->push(std::move(edit));
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
  const std::vector<Line> before = split_lines(original);
  const std::vector<Line> after = split_lines(formatted);
  const usize asked_start = document.index().byte_offset(original, start);
  const usize asked_end = document.index().byte_offset(original, end);
  const usize n = before.size();
  const usize m = after.size();
  if (n != 0 && m != 0 && n > 4000000 / m) {
    append_edit(document, original, original, formatted, 0, original.size(),
                asked_start, asked_end, &edits);
    return edits;
  }
  std::vector<std::vector<u32>> lcs(n + 1, std::vector<u32>(m + 1, 0));
  for (usize i = n; i > 0; --i) {
    for (usize j = m; j > 0; --j) {
      lcs[i - 1][j - 1] = before[i - 1].text == after[j - 1].text
                              ? lcs[i][j] + 1
                              : std::max(lcs[i][j - 1], lcs[i - 1][j]);
    }
  }
  usize i = 0;
  usize j = 0;
  usize hunk_before = 0;
  usize hunk_after = 0;
  const auto flush = [&]() {
    if (hunk_before > i || hunk_after > j ||
        (hunk_before == i && hunk_after == j)) {
      return;
    }
    const usize from =
        hunk_before < n ? before[hunk_before].start : original.size();
    const usize to = i < n ? before[i].start : original.size();
    append_edit(document, original, original.substr(from, to - from),
                join_lines(after, hunk_after, j), from, to, asked_start,
                asked_end, &edits);
  };
  while (i < n && j < m) {
    if (before[i].text == after[j].text) {
      flush();
      ++i;
      ++j;
      hunk_before = i;
      hunk_after = j;
    } else if (lcs[i + 1][j] >= lcs[i][j + 1]) {
      ++i;
    } else {
      ++j;
    }
  }
  flush();
  return edits;
}

}  // namespace lsp
