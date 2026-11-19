// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "fpag/base/result.h"

namespace ssg {

// A parser for the Markdown subset the site is written in. The subset
// is deliberately small and documented here, because it is what the
// guide's authors can rely on:
//
//   - paragraphs, wrapped over any number of lines and joined with a space
//   - ATX headings `#` through `######`
//   - fenced code blocks with an info string; `alcy` fences are complete
//     programs and are compiled at build time
//   - unordered and ordered lists, one level; an indented continuation
//     line belongs to the item above it
//   - blockquotes, one paragraph per quote
//   - tables: a header row, a `---` separator, a row per line; cells may
//     hold inline markup; pipes in code spans are not delimiters
//   - inline `code`, **strong**, *emphasis*, and `[text](url)` links
//
// Not in the subset: raw HTML (it is escaped like any other text), images,
// nested lists, and indented code blocks. A block that is not in the subset
// is a paragraph, so the failure mode is visible, not fatal.
//
// Blocks carry their one-based starting line so a build error can name a
// place in the source file; inline markup is rendered by `render_inline`
// with the caller's link resolver, not at parse time.

enum class BlockType : u8 {
  Heading,
  Paragraph,
  Code,
  List,
  Blockquote,
  Table,
};

// A table cell alignment, from the separator line.
enum class CellAlign : u8 {
  None,
  Left,
  Center,
  Right,
};

struct Block {
  BlockType type = BlockType::Paragraph;
  // The one-based line the block starts on.
  usize line = 0;
  // Heading level.
  i32 level = 0;
  // A code block's info string; a table's heading uses `text` for nothing.
  std::string language;
  // The heading, paragraph, blockquote, or code body text.
  std::string text;
  // The list's items.
  std::vector<std::string> items;
  bool ordered = false;
  // Table: row zero is the header; every row has the same width.
  std::vector<std::vector<std::string>> rows;
  // Per column; size == row width.
  std::vector<CellAlign> aligns;
};

using Blocks = std::vector<Block>;

// Renders a link URL; the default keeps the URL the page already names.
using ResolveLink = std::string (*)(std::string_view url, void* context);

base::Result<Blocks, std::string> parse_markdown(std::string_view source);

std::string render_inline(std::string_view text,
                          ResolveLink resolve = nullptr,
                          void* resolve_context = nullptr);

std::string escape_html(std::string_view text);
std::string escape_attribute(std::string_view text);

// The table cell split, exposed for the tests: the same routine the
// block parser uses.
std::vector<std::string> split_table_cells(std::string_view line);

}  // namespace ssg
