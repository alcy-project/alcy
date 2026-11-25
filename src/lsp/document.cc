// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/document.h"

#include <algorithm>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "fpag/base/numeric.h"

namespace lsp {

namespace {

// UTF-8 sequence length from its leading byte. A stray continuation byte
// counts as one so the walk always advances.
usize utf8_length(char byte) {
  const u8 value = static_cast<u8>(byte);
  if (value < 0x80) {
    return 1;
  }
  if ((value & 0xE0) == 0xC0) {
    return 2;
  }
  if ((value & 0xF0) == 0xE0) {
    return 3;
  }
  if ((value & 0xF8) == 0xF0) {
    return 4;
  }
  return 1;
}

// A code point outside the BMP takes two UTF-16 code units; everything
// UTF-8 can encode in at most three bytes takes one.
u32 utf16_units(char byte) {
  return static_cast<u8>(byte) >= 0xF0 ? 2 : 1;
}

bool is_line_break(char c) {
  return c == '\n' || c == '\r';
}

// True when a line starts immediately after `i`: a newline always does,
// a carriage return only without the newline that would pair with it.
bool line_starts_after(std::string_view text, usize i) {
  if (text[i] == '\n') {
    return true;
  }
  return text[i] == '\r' && (i + 1 == text.size() || text[i + 1] != '\n');
}

}  // namespace

u32 PositionIndex::units(char byte) const {
  if (encoding_ == PositionEncoding::Utf8) {
    return static_cast<u32>(utf8_length(byte));
  }
  return utf16_units(byte);
}

void PositionIndex::reset(std::string_view text) {
  line_starts_.clear();
  line_starts_.push_back(0);
  for (usize i = 0; i < text.size(); ++i) {
    // A lone carriage return starts a line; the one before a newline does
    // not, so "\r\n" counts as a single break.
    if (line_starts_after(text, i)) {
      line_starts_.push_back(static_cast<u32>(i + 1));
    }
  }
}

void PositionIndex::apply(std::string_view text,
                          usize start,
                          usize end,
                          usize new_len) {
  const usize lo = start > 0 ? start - 1 : 0;
  const i64 delta = static_cast<i64>(new_len) - static_cast<i64>(end - start);
  const usize scan_end = start + new_len;

  usize first = 0;
  while (first < line_starts_.size() && line_starts_[first] <= lo) {
    ++first;
  }
  usize last = first;
  while (last < line_starts_.size() && line_starts_[last] <= end) {
    ++last;
  }
  std::vector<u32> inserted;
  for (usize i = lo; i < scan_end && i < text.size(); ++i) {
    if (line_starts_after(text, i)) {
      inserted.push_back(static_cast<u32>(i + 1));
    }
  }
  line_starts_.erase(line_starts_.begin() + static_cast<isize>(first),
                     line_starts_.begin() + static_cast<isize>(last));
  line_starts_.insert(line_starts_.begin() + static_cast<isize>(first),
                      inserted.begin(), inserted.end());
  for (usize i = first + inserted.size(); i < line_starts_.size(); ++i) {
    line_starts_[i] =
        static_cast<u32>(static_cast<i64>(line_starts_[i]) + delta);
  }
}

usize PositionIndex::byte_offset(std::string_view text,
                                 Position position) const {
  if (line_starts_.empty()) {
    return 0;
  }
  if (position.line >= line_starts_.size()) {
    return text.size();
  }
  const usize line_start = line_starts_[position.line];
  const usize line_limit = position.line + 1 < line_starts_.size()
                               ? line_starts_[position.line + 1]
                               : text.size();
  usize offset = line_start;
  u32 count = 0;
  while (offset < line_limit && count < position.character) {
    if (is_line_break(text[offset])) {
      break;
    }
    const usize length = utf8_length(text[offset]);
    count += this->units(text[offset]);
    offset += length;
  }
  return std::min(offset, line_limit);
}

Position PositionIndex::position_at(std::string_view text, usize offset) const {
  if (line_starts_.empty()) {
    return Position{};
  }
  const usize target = std::min(offset, text.size());
  const auto it = std::upper_bound(line_starts_.begin(), line_starts_.end(),
                                   static_cast<u32>(target));
  const usize line = static_cast<usize>(it - line_starts_.begin()) - 1;
  usize cursor = line_starts_[line];
  u32 count = 0;
  while (cursor < target) {
    if (is_line_break(text[cursor])) {
      break;
    }
    const usize length = utf8_length(text[cursor]);
    if (cursor + length > target) {
      break;
    }
    count += this->units(text[cursor]);
    cursor += length;
  }
  return Position{static_cast<u32>(line), count};
}

Document::Document(std::string uri,
                   std::string language_id,
                   i64 version,
                   std::string text)
    : uri_(std::move(uri)),
      language_id_(std::move(language_id)),
      version_(version),
      text_(std::move(text)) {
  rebuild();
}

void Document::apply(std::span<const ContentChange> changes, i64 version) {
  for (const ContentChange& change : changes) {
    if (!change.has_range) {
      text_ = change.text;
      index_.reset(text_);
    } else {
      const usize start = index_.byte_offset(text_, change.range.start);
      const usize end = index_.byte_offset(text_, change.range.end);
      if (start <= end && end <= text_.size()) {
        text_.replace(start, end - start, change.text);
        index_.apply(text_, start, end, change.text.size());
      }
    }
    // Each change addresses the document the previous one produced.
  }
  version_ = version;
}

void Document::rebuild() {
  index_.reset(text_);
}

Document& DocumentStore::open(std::string uri,
                              std::string language_id,
                              i64 version,
                              std::string text) {
  Document document(uri, std::move(language_id), version, std::move(text));
  auto it = documents_.insert_or_assign(std::move(uri), std::move(document));
  return it.first->second;
}

Document* DocumentStore::find(std::string_view uri) {
  const auto it = documents_.find(uri);
  return it == documents_.end() ? nullptr : &it->second;
}

const Document* DocumentStore::find(std::string_view uri) const {
  const auto it = documents_.find(uri);
  return it == documents_.end() ? nullptr : &it->second;
}

bool DocumentStore::close(std::string_view uri) {
  return documents_.erase(std::string(uri)) != 0;
}

}  // namespace lsp
