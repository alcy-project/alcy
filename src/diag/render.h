// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "diag/diagnostic.h"
#include "fmt/format.h"
#include "fpag/base/numeric.h"
#include "i18n/language.h"
#include "source/source.h"

namespace diag {
// Zero-copy source lookup for the renderer: both views borrow from storage
// owned by the caller (e.g. SourceManager's mapped files).
struct SourceText {
  std::string_view name;
  std::string_view bytes;
};

// The 1-based line and display column a byte offset falls on, measured
// the way the renderer's caret is: UTF-8 code points, with a tab
// advancing to the next tab stop. An offset past the end clamps to the
// end of the buffer.
struct LineColumn {
  u32 line = 1;
  u32 column = 1;
};

LineColumn line_column(std::string_view bytes, u32 offset);

// The line and column of offsets in one text, each answer continuing the
// scan the previous one stopped at. A caller that asks in source order
// pays one pass over the text; one that asks out of order, or about
// another text, falls back to the scan line_column makes. The text stays
// the caller's and has to outlive the cursor.
class LineCursor {
 public:
  // Counts a line and a column the way line_column does, and clamps an
  // offset past the end the same way.
  LineColumn locate(std::string_view bytes, u32 offset);

 private:
  // The text the fields below describe.
  std::string_view scanned_;
  // The offset the scan reached, and the line and line start it found
  // there.
  u32 offset_ = 0;
  u32 line_ = 1;
  u32 line_start_ = 0;
};

// Returns the source text for a file id, or std::nullopt when the id does
// not name a known file - distinct from a known file whose bytes are
// empty. Must never allocate.
using SourceFetch = std::optional<SourceText> (*)(source::FileId file_id,
                                                  const void* ctx);

// Presentation choices supplied by the output layer. The renderer does not
// inspect the terminal or environment.
struct RenderOptions {
  bool color = false;
  // Language the chrome is written in: the severity word, the note that
  // labels a secondary span, and the stand-in for a file the manager
  // cannot name. The message itself was composed in this language when it
  // was emitted, so a bag and these options disagree only if a caller
  // renders one bag under two languages.
  i18n::Language language = i18n::Language::EnUs;
};

// One line of a source text as the renderer reads it: the line the offset
// falls on, the column it sits at, where the line starts, and the line's
// characters.
struct SourceLine {
  // 1-based line number, and the display column line_column reports.
  u32 number = 1;
  u32 column = 1;
  // Byte offset the line starts at, so a caller converting a byte length
  // does not have to reverse the column arithmetic.
  u32 start = 0;
  std::string_view text;
};

// Where the lines of each text start, so the place of every diagnostic in a
// bag costs a search rather than a scan from the top of the file. A text is
// indexed from its second question on: the one diagnostic a file usually
// carries pays the scan it would have paid anyway and no memory. The texts
// stay the caller's and have to outlive the index.
class LineIndex {
 public:
  // Answers the way line_column does, and clamps an offset past the end the
  // same way.
  SourceLine line_at(source::FileId file_id,
                     std::string_view bytes,
                     u32 offset);

 private:
  struct Text {
    source::FileId file_id = source::UNKNOWN_FILE;
    std::string_view bytes;
    // Byte offset of every line start, ascending, or empty until the text
    // is asked about a second time.
    std::vector<u32> starts;
    // The last place on a line that was asked about, and the column it sits
    // at. A bag walks one line left to right, so the next question there
    // continues from it instead of rescanning the line.
    u32 cursor_start = 0;
    u32 cursor_offset = 0;
    u32 cursor_column = 1;
    bool cursor_valid = false;
  };

  std::vector<Text> texts_;
};

// Renders one diagnostic into out. Snippets are single-line
// (multi-line spans are clipped to their first line); secondary labels
// render as location lines. Only out itself may grow; inputs are only read.
// Passing an index answers positions from it; without one every span is
// located by scanning the text.
void render(const Diagnostic& diag,
            fmt::memory_buffer& out,
            const RenderOptions& options = {},
            SourceFetch fetch = nullptr,
            const void* ctx = nullptr,
            LineIndex* lines = nullptr);

// `render` for a caller that wants the text rather than a buffer, and has
// no source to quote: the marker and the message, and nothing else. A
// message from outside a check area - a rejected command line, a flag
// combination the config check turned down - is exactly that, and a
// buffer it has to copy out of is a step with no other purpose.
std::string render(const Diagnostic& diag, const RenderOptions& options = {});

}  // namespace diag
