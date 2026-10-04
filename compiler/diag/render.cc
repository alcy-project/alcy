// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "diag/render.h"

#include <iterator>
#include <optional>
#include <string>
#include <string_view>

#include "diag/diagnostic.h"
#include "diag/span.h"
#include "diag/stage.h"
#include "fmt/core.h"
#include "fmt/format.h"
#include "fpag/base/numeric.h"
#include "fpag/term/style.h"
#include "i18n/language.h"
#include "i18n/messages.h"

namespace diag {

namespace {

std::string_view severity_text(Severity severity, i18n::Language language) {
  switch (severity) {
    case Severity::Error:
      return i18n::text<i18n::Key::RenderSeverityError>(language);
    case Severity::Warning:
      return i18n::text<i18n::Key::RenderSeverityWarning>(language);
    case Severity::Note:
      return i18n::text<i18n::Key::RenderSeverityNote>(language);
  }
}

constexpr char severity_code(Severity severity) {
  switch (severity) {
    case Severity::Error: return 'E';
    case Severity::Warning: return 'W';
    case Severity::Note: return 'N';
  }
}

constexpr std::string_view severity_color(Severity severity) {
  switch (severity) {
    case Severity::Error: return term::FG_RED;
    case Severity::Warning: return term::FG_YELLOW;
    case Severity::Note: return term::FG_CYAN;
  }
}

struct LineInfo {
  u32 line = 1;
  // 1-based display column: UTF-8 code points, with a tab advancing to
  // the next multiple of TAB_WIDTH. Byte columns would put the caret
  // after every multi-byte character and before every expanded tab.
  u32 col = 1;
  // Byte offset where the line starts, so callers converting a
  // byte-relative length do not have to reverse the column arithmetic.
  u32 start = 0;
  std::string_view text;
};

constexpr u32 TAB_WIDTH = 4;

// Widest caret run rendered. Past this the run says nothing extra about
// where the span starts, and the line above already shows the extent.
constexpr u32 MAX_CARET_RUN = 80;

// The part of a line a snippet shows. A line can be arbitrarily long - an
// identifier is legal input at any size - so the snippet keeps a window
// around the caret and marks where it cut.
struct SnippetWindow {
  usize begin = 0;
  usize end = 0;
  bool clipped_left = false;
  bool clipped_right = false;
};

// The window's width in bytes. Bytes rather than columns because a cut
// is a reading aid, not a layout, and the caret arithmetic measures its
// own columns from the window's start.
constexpr usize SNIPPET_MAX_BYTES = 240;

SnippetWindow snippet_window(std::string_view line, usize caret) {
  if (line.size() <= SNIPPET_MAX_BYTES) {
    return {0, line.size(), false, false};
  }
  // The caret stays visible with more of the line before it than after,
  // which is where a reader looks first.
  usize begin =
      caret > SNIPPET_MAX_BYTES / 3 ? caret - SNIPPET_MAX_BYTES / 3 : 0;
  usize end = begin + SNIPPET_MAX_BYTES;
  if (end > line.size()) {
    end = line.size();
    begin = end - SNIPPET_MAX_BYTES;
  }
  // A cut inside a multi-byte character would render its tail alone, so
  // the left edge steps over continuation bytes. The right edge is
  // exclusive and may cut after a character's first byte, which keeps it
  // whole rather than dropping it.
  while (begin > 0 &&
         (static_cast<u8>(line[begin]) & 0xC0) == 0x80) {
    ++begin;
  }
  while (end < line.size() &&
         (static_cast<u8>(line[end]) & 0xC0) == 0x80) {
    ++end;
  }
  return {begin, end, begin > 0, end < line.size()};
}

// Display columns spanned by the bytes in [line_start, offset).
u32 display_column(std::string_view bytes, u32 line_start, u32 offset) {
  u32 col = 1;
  for (u32 i = line_start; i < offset && i < bytes.size(); ++i) {
    // A UTF-8 continuation byte continues the character before it, so
    // it occupies no column of its own.
    if ((static_cast<u8>(bytes[i]) & 0xC0) == 0x80) {
      continue;
    }
    col = bytes[i] == '\t' ? ((col - 1) / TAB_WIDTH + 1) * TAB_WIDTH + 1
                           : col + 1;
  }
  return col;
}

// Locates the 1-based line/column of a byte offset and extracts the line.
// Out-of-range offsets clamp to the end of the buffer.
LineInfo locate(std::string_view bytes, u32 offset) {
  if (offset > bytes.size()) {
    offset = static_cast<u32>(bytes.size());
  }
  u32 line = 1;
  u32 line_start = 0;
  for (u32 i = 0; i < offset; ++i) {
    if (bytes[i] == '\n') {
      ++line;
      line_start = i + 1;
    }
  }
  u32 line_end = static_cast<u32>(bytes.size());
  for (u32 i = line_start; i < static_cast<u32>(bytes.size()); ++i) {
    if (bytes[i] == '\n') {
      line_end = i;
      break;
    }
  }
  return {.line = line,
          .col = display_column(bytes, line_start, offset),
          .start = line_start,
          .text = bytes.substr(line_start, line_end - line_start)};
}

// Renders source text for the snippet, expanding tabs to the same width
// the column arithmetic assumes so the caret stays under its character.
// Tab stops count from the start of the line, matching display_column,
// not from wherever the output buffer happens to begin.
void append_expanded(fmt::memory_buffer& out, std::string_view text) {
  u32 col = 1;
  for (const char c : text) {
    if (c != '\t') {
      out.push_back(c);
      // A UTF-8 continuation byte continues the character before it, so
      // it occupies no column of its own; display_column counts the same
      // way, and a tab after a multibyte character would otherwise
      // advance from a column one too far right.
      if ((static_cast<u8>(c) & 0xC0) != 0x80) {
        ++col;
      }
      continue;
    }
    // The tab advances to the next stop from the column it starts at:
    // the spaces appended are the distance to that stop, not the stop's
    // absolute column.
    const u32 next = ((col - 1) / TAB_WIDTH + 1) * TAB_WIDTH + 1;
    for (u32 i = col; i < next; ++i) {
      out.push_back(' ');
    }
    col = next;
  }
}

u32 decimal_width(u32 value) {
  u32 width = 1;
  while (value >= 10) {
    value /= 10;
    ++width;
  }
  return width;
}

void append_text(fmt::memory_buffer& out, std::string_view text) {
  if (text.empty()) {
    return;
  }
  out.append(text.data(), text.data() + text.size());
}

void begin_style(fmt::memory_buffer& out,
                 bool enabled,
                 std::string_view color,
                 bool bold = false) {
  if (!enabled) {
    return;
  }
  if (bold) {
    append_text(out, term::BOLD);
  }
  append_text(out, color);
}

void end_style(fmt::memory_buffer& out, bool enabled) {
  if (enabled) {
    append_text(out, term::RESET);
  }
}

void append_colored(fmt::memory_buffer& out,
                    bool enabled,
                    std::string_view color,
                    std::string_view text) {
  if (!enabled) {
    append_text(out, text);
    return;
  }
  begin_style(out, true, color);
  append_text(out, text);
  end_style(out, true);
}

void write_gutter(fmt::memory_buffer& out, u32 width) {
  for (u32 i = 0; i < width; ++i) {
    out.push_back(' ');
  }
  fmt::format_to(std::back_inserter(out), " |");
}

// One marker for every severity, in colour or not: the word and the code
// the diagnostic carries, and nothing else. A message from outside a
// check area opens with `error: `, which is the honest form when no code
// was ever allocated for it.
void append_marker(fmt::memory_buffer& out,
                   const Diagnostic& diag,
                   const RenderOptions& options) {
  const bool color = options.color;
  if (color) {
    begin_style(out, true, severity_color(diag.severity), true);
  }
  fmt::format_to(std::back_inserter(out), "{}",
                 severity_text(diag.severity, options.language));
  if (diag.code.has_value()) {
    // Severity, component, id: `error[EA001]`. The component's letter is
    // the only part that needs the registry to interpret, and the id is
    // padded to three digits so codes sort in the order they were
    // assigned - which is the order a bug report lists them in.
    fmt::format_to(std::back_inserter(out), "[{}{}{:03}]",
                   severity_code(diag.severity), stage_letter(diag.code->stage),
                   diag.code->id);
  }
  if (color) {
    end_style(out, true);
  }
  fmt::format_to(std::back_inserter(out), ": {}\n", diag.message);
}

}  // namespace

std::string render(const Diagnostic& diag, const RenderOptions& options) {
  fmt::memory_buffer out;
  render(diag, out, options);
  return std::string(out.data(), out.size());
}

void render(const Diagnostic& diag,
            fmt::memory_buffer& out,
            const RenderOptions& options,
            SourceFetch fetch,
            const void* ctx) {
  append_marker(out, diag, options);

  if (!diag.has_primary_span) {
    return;
  }

  std::optional<SourceText> fetched;
  if (fetch != nullptr) {
    fetched = fetch(diag.primary_span.file, ctx);
  }
  const SourceText source = fetched.value_or(SourceText{});
  const std::string_view name =
      source.name.empty()
          ? i18n::text<i18n::Key::RenderUnknownFile>(options.language)
          : source.name;

  fmt::format_to(std::back_inserter(out), " --> ");
  append_colored(out, options.color, term::FG_CYAN, name);
  // No bytes, or a span past the end of the file: show the raw offset
  // rather than fabricate a line/column the text cannot support.
  if (source.bytes.empty() || diag.primary_span.offset > source.bytes.size()) {
    fmt::format_to(std::back_inserter(out), ":{}\n", diag.primary_span.offset);
    return;
  }

  const LineInfo info = locate(source.bytes, diag.primary_span.offset);
  fmt::format_to(std::back_inserter(out), ":{}:{}\n", info.line, info.col);

  // Caret run clipped to the rendered snippet. locate() clamps
  // out-of-range offsets; mirror that here.
  u32 line_offset = diag.primary_span.offset;
  if (line_offset > source.bytes.size()) {
    line_offset = static_cast<u32>(source.bytes.size());
  }
  // A line can be arbitrarily long - an identifier is legal input at any
  // size - and printing all of one buries the message it belongs to.
  const SnippetWindow window =
      snippet_window(info.text, line_offset - info.start);
  const u32 base_offset = info.start + static_cast<u32>(window.begin);

  const u32 gutter = decimal_width(info.line);
  write_gutter(out, gutter);
  fmt::format_to(std::back_inserter(out), "\n");
  begin_style(out, options.color, term::FG_BLUE);
  fmt::format_to(std::back_inserter(out), "{}", info.line);
  end_style(out, options.color);
  fmt::format_to(std::back_inserter(out), " | ");
  if (window.clipped_left) {
    append_text(out, "...");
  }
  append_expanded(out,
                  info.text.substr(window.begin, window.end - window.begin));
  if (window.clipped_right) {
    append_text(out, "...");
  }
  fmt::format_to(std::back_inserter(out), "\n");

  // The run is measured in display columns, so a span covering
  // multi-byte characters underlines as wide as it looks. Columns count
  // from where the snippet starts, and the left ellipsis is not columns
  // of its own text.
  const u32 snippet_shift = window.clipped_left ? 3 : 0;
  const u32 start_col =
      display_column(source.bytes, base_offset, line_offset) + snippet_shift;
  const u32 snippet_end = info.start + static_cast<u32>(window.end);
  u32 caret_end = line_offset + diag.primary_span.length;
  if (caret_end > snippet_end) {
    caret_end = snippet_end;
  }
  const u32 end_col =
      display_column(source.bytes, base_offset, caret_end) + snippet_shift;
  u32 carets = end_col > start_col ? end_col - start_col : 0;
  if (carets == 0) {
    carets = 1;
  }
  // A span can cover a whole line - a rejected deeply nested expression
  // spans thousands of columns - and a caret per column buries the
  // message in noise, so the run is capped at a readable width.
  if (carets > MAX_CARET_RUN) {
    carets = MAX_CARET_RUN;
  }
  write_gutter(out, gutter);
  out.push_back(' ');
  for (u32 i = 1; i < start_col; ++i) {
    out.push_back(' ');
  }
  begin_style(out, options.color, severity_color(diag.severity));
  for (u32 i = 0; i < carets; ++i) {
    out.push_back('^');
  }
  end_style(out, options.color);
  out.push_back('\n');

  for (u32 i = 0; i < diag.label_count; ++i) {
    const Label& label = diag.labels[i];
    std::optional<SourceText> fetched_label;
    if (fetch != nullptr) {
      fetched_label = fetch(label.span.file, ctx);
    }
    const SourceText label_source = fetched_label.value_or(SourceText{});
    const std::string_view label_name =
        label_source.name.empty()
            ? i18n::text<i18n::Key::RenderUnknownFile>(options.language)
            : label_source.name;
    fmt::format_to(std::back_inserter(out), " = ");
    begin_style(out, options.color, term::FG_CYAN, true);
    fmt::format_to(std::back_inserter(out), "{}",
                   i18n::text<i18n::Key::RenderSeverityNote>(options.language));
    end_style(out, options.color);
    fmt::format_to(std::back_inserter(out), ": {} --> ", label.message);
    append_colored(out, options.color, term::FG_CYAN, label_name);
    if (label_source.bytes.empty() ||
        label.span.offset > label_source.bytes.size()) {
      fmt::format_to(std::back_inserter(out), ":{}\n", label.span.offset);
      continue;
    }
    const LineInfo label_info = locate(label_source.bytes, label.span.offset);
    fmt::format_to(std::back_inserter(out), ":{}:{}\n", label_info.line,
                   label_info.col);
  }
}

}  // namespace diag
