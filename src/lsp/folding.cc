// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/folding.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cst/cst.h"
#include "cst/lex.h"
#include "fpag/base/numeric.h"
#include "lsp/document.h"
#include "text/json.h"

namespace lsp {

namespace {

struct FoldRange {
  u32 start_line = 0;
  u32 end_line = 0;
  const char* kind = nullptr;
};

// A closed brace pair covers [offset, offset + length): the opener is
// its first byte and the closer its last, so the two lines a fold spans
// are read from those ends.
void collect_braces(const cst::Node& node,
                    const Document& document,
                    std::vector<FoldRange>* out) {
  if (node.kind == cst::NodeKind::Group && node.has_closer &&
      node.group_role == cst::GroupRole::Brace && node.length > 0) {
    const std::string_view text = document.text();
    const usize close = static_cast<usize>(node.offset) + node.length - 1;
    if (close < text.size()) {
      const u32 open_line =
          document.index().position_at(text, node.offset).line;
      const u32 close_line = document.index().position_at(text, close).line;
      if (close_line > open_line) {
        out->emplace_back(open_line, close_line, nullptr);
      }
    }
  }
  for (const cst::Node& kid : node.kids) {
    collect_braces(kid, document, out);
  }
}

void collect_comments(const cst::Node& node,
                      std::vector<std::pair<u32, u32>>* out) {
  if (node.kind == cst::NodeKind::Comment) {
    out->emplace_back(node.offset, node.length);
  }
  for (const cst::Node& kid : node.kids) {
    collect_comments(kid, out);
  }
}

// Whether only one line break and whitespace sit between two comments,
// so a run of them folds as one region. Code in the gap ends the run.
bool blank_between(std::string_view text, usize from, usize to) {
  if (from > to || to > text.size()) {
    return false;
  }
  usize newlines = 0;
  for (usize i = from; i < to; ++i) {
    const char c = text[i];
    if (c == '\n') {
      ++newlines;
    } else if (c != ' ' && c != '\t' && c != '\r') {
      return false;
    }
  }
  return newlines <= 1;
}

}  // namespace

text::JsonValue document_folding_ranges(const Document& document) {
  const std::vector<cst::Lexeme> lexemes = cst::lex(document.text());
  const cst::Node root = cst::build_cst(lexemes);
  const std::string_view text = document.text();

  std::vector<FoldRange> ranges;
  collect_braces(root, document, &ranges);

  std::vector<std::pair<u32, u32>> comments;
  collect_comments(root, &comments);
  usize index = 0;
  while (index < comments.size()) {
    usize last = index;
    while (last + 1 < comments.size() &&
           blank_between(text, comments[last].first + comments[last].second,
                         comments[last + 1].first)) {
      ++last;
    }
    const usize end = comments[last].first + comments[last].second;
    const u32 start_line =
        document.index().position_at(text, comments[index].first).line;
    const u32 end_line =
        document.index().position_at(text, end > 0 ? end - 1 : 0).line;
    if (end_line > start_line) {
      ranges.emplace_back(start_line, end_line, "comment");
    }
    index = last + 1;
  }

  std::sort(ranges.begin(), ranges.end(),
            [](const FoldRange& left, const FoldRange& right) {
              if (left.start_line != right.start_line) {
                return left.start_line < right.start_line;
              }
              return left.end_line < right.end_line;
            });

  text::JsonValue out = text::JsonValue::make_array();
  for (const FoldRange& range : ranges) {
    text::JsonValue entry = text::JsonValue::make_object();
    entry.set("startLine", text::JsonValue::make_number(range.start_line));
    entry.set("endLine", text::JsonValue::make_number(range.end_line));
    if (range.kind != nullptr) {
      entry.set("kind", text::JsonValue::make_string(std::string(range.kind)));
    }
    out.push(std::move(entry));
  }
  return out;
}

}  // namespace lsp
