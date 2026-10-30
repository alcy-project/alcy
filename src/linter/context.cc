// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "linter/context.h"

#include <optional>
#include <string_view>

#include "diag/span.h"
#include "source/source.h"

namespace linter {

namespace {

bool is_word_char(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_' || c == '-';
}

std::string_view trim_left(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t' ||
                           text.front() == '\r')) {
    text.remove_prefix(1);
  }
  return text;
}

std::string_view trim_right(std::string_view text) {
  while (!text.empty() &&
         (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
    text.remove_suffix(1);
  }
  return text;
}

std::string_view trim(std::string_view text) {
  return trim_right(trim_left(text));
}

// The text of the line's `//` comment, or empty when the line has none.
// The language has no block comment, so the first `//` starts the only
// comment a line can carry.
std::string_view comment_of(std::string_view line) {
  const usize at = line.find("//");
  if (at == std::string_view::npos) {
    return {};
  }
  return line.substr(at + 2);
}

// Whether the comment is `directive` and whether it names `rule`. A
// directive with no `: ` list silences every rule; a list is
// comma-separated ids and may be followed by `--` and the author's
// reason, which is read as prose.
bool directive_says(std::string_view comment,
                    std::string_view directive,
                    std::string_view rule) {
  comment = trim_left(comment);
  if (!comment.starts_with(directive)) {
    return false;
  }
  comment.remove_prefix(directive.size());
  // The directive is a word, not a prefix of a longer one:
  // `alcy-lint-ignore-next-line` is not `alcy-lint-ignore`.
  if (!comment.empty() && is_word_char(comment.front())) {
    return false;
  }
  comment = trim_left(comment);
  if (comment.empty() || comment.front() != ':') {
    return true;
  }
  comment.remove_prefix(1);
  const usize reason = comment.find("--");
  if (reason != std::string_view::npos) {
    comment = comment.substr(0, reason);
  }
  bool named_any = false;
  while (!comment.empty()) {
    const usize comma = comment.find(',');
    const std::string_view item = trim(
        comma == std::string_view::npos ? comment : comment.substr(0, comma));
    if (!item.empty()) {
      named_any = true;
      if (item == rule || item == "all") {
        return true;
      }
    }
    if (comma == std::string_view::npos) {
      break;
    }
    comment.remove_prefix(comma + 1);
  }
  // A colon with nothing after it reads as the bare directive.
  return !named_any;
}

// The line holding `offset`, with its first byte written to `begin`.
std::string_view line_at(std::string_view text, usize offset, usize* begin) {
  usize start = 0;
  if (offset > 0) {
    const usize previous = text.rfind('\n', offset - 1);
    start = previous == std::string_view::npos ? 0 : previous + 1;
  }
  const usize end = text.find('\n', offset);
  *begin = start;
  return text.substr(
      start, (end == std::string_view::npos ? text.size() : end) - start);
}

}  // namespace

bool suppressed(const source::SourceManager& sources,
                diag::Span span,
                std::string_view rule) {
  const std::optional<std::string_view> bytes = sources.bytes(span.file);
  if (!bytes.has_value() || span.offset > bytes->size()) {
    return false;
  }
  const std::string_view text = *bytes;
  usize line_begin = 0;
  const std::string_view line = line_at(text, span.offset, &line_begin);
  if (directive_says(comment_of(line), "alcy-lint-ignore", rule)) {
    return true;
  }
  // The line above can silence this one through the next-line spelling.
  // A first line has nothing above it.
  if (line_begin < 2) {
    return false;
  }
  usize above_begin = 0;
  const std::string_view above = line_at(text, line_begin - 2, &above_begin);
  return directive_says(comment_of(above), "alcy-lint-ignore-next-line", rule);
}

}  // namespace linter
