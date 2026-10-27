// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <functional>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "fpag/base/numeric.h"

namespace lsp {

// An LSP text position: zero-based line, and a character counted in
// UTF-16 code units from the line start.
struct Position {
  u32 line = 0;
  u32 character = 0;

  bool operator==(const Position&) const = default;
};

struct Range {
  Position start;
  Position end;
};

// One entry of a `textDocument/didChange`. An absent range replaces the
// whole document.
struct ContentChange {
  bool has_range = false;
  Range range;
  std::string text;
};

// Line starts over one buffer, for translating between LSP positions and
// the byte offsets the compiler reports. The text is passed in rather
// than borrowed, so a moved or reassigned buffer cannot leave a view
// behind.
class PositionIndex {
 public:
  void reset(std::string_view text);

  // Byte offset of `position`. A character past the line end clamps to
  // the line end, and a line past the text clamps to the text end.
  usize byte_offset(std::string_view text, Position position) const;

  // Position of `offset`. An offset past the end clamps, and an offset
  // inside a code point rounds down to its start.
  Position position_at(std::string_view text, usize offset) const;

 private:
  std::vector<u32> line_starts_;
};

// One open document: its buffer, its version, and the line index over it.
class Document {
 public:
  Document(std::string uri,
           std::string language_id,
           i64 version,
           std::string text);

  const std::string& uri() const { return uri_; }
  const std::string& language_id() const { return language_id_; }
  i64 version() const { return version_; }
  std::string_view text() const { return text_; }
  const PositionIndex& index() const { return index_; }

  // Applies `changes` in order; each one sees the document the previous
  // ones left. Records `version` once they are applied.
  void apply(std::span<const ContentChange> changes, i64 version);

 private:
  void rebuild();

  std::string uri_;
  std::string language_id_;
  i64 version_ = 0;
  std::string text_;
  PositionIndex index_;
};

// The documents a client has opened, keyed by URI. A session holds a
// handful, so a map is the whole index.
class DocumentStore {
 public:
  Document& open(std::string uri,
                 std::string language_id,
                 i64 version,
                 std::string text);
  Document* find(std::string_view uri);
  const Document* find(std::string_view uri) const;
  bool close(std::string_view uri);

  const std::map<std::string, Document, std::less<>>& documents() const {
    return documents_;
  }

 private:
  std::map<std::string, Document, std::less<>> documents_;
};

}  // namespace lsp
