// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "diag/render.h"

#include <iterator>
#include <optional>
#include <string_view>

#include "diag/diagnostic.h"
#include "diag/span.h"
#include "fmt/format.h"
#include "fpag/base/numeric.h"
#include "fpag/term/style.h"

namespace diag {

namespace {

constexpr std::string_view severity_text(Severity severity) {
  switch (severity) {
    case Severity::Error: return "error";
    case Severity::Warning: return "warning";
    case Severity::Note: return "note";
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
    case Severity::Error: return term::kRed;
    case Severity::Warning: return term::kYellow;
    case Severity::Note: return term::kCyan;
  }
}

struct LineInfo {
  u32 line = 1;
  u32 col = 1;
  std::string_view text;
};

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
          .col = offset - line_start + 1,
          .text = bytes.substr(line_start, line_end - line_start)};
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
    append_text(out, term::kBold);
  }
  append_text(out, color);
}

void end_style(fmt::memory_buffer& out, bool enabled) {
  if (enabled) {
    append_text(out, term::kReset);
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

}  // namespace

void render(const Diagnostic& diag,
            fmt::memory_buffer& out,
            const RenderOptions& options,
            SourceFetch fetch,
            const void* ctx) {
  const std::string_view sev = severity_text(diag.severity);
  if (options.color) {
    begin_style(out, true, severity_color(diag.severity), true);
    fmt::format_to(std::back_inserter(out), "{}[{}{}]", sev,
                   severity_code(diag.severity), diag.code);
    end_style(out, true);
    fmt::format_to(std::back_inserter(out), ": {}\n", diag.message);
  } else {
    fmt::format_to(std::back_inserter(out), "{}[{}{}]: {}\n", sev,
                   severity_code(diag.severity), diag.code, diag.message);
  }

  if (!diag.has_primary_span) {
    return;
  }

  std::optional<SourceText> fetched;
  if (fetch != nullptr) {
    fetched = fetch(diag.primary_span.file, ctx);
  }
  const SourceText source = fetched.value_or(SourceText{});
  const std::string_view name =
      source.name.empty() ? "[unknown file]" : source.name;

  fmt::format_to(std::back_inserter(out), " --> ");
  append_colored(out, options.color, term::kCyan, name);
  // No bytes, or a span past the end of the file: show the raw offset
  // rather than fabricate a line/column the text cannot support.
  if (source.bytes.empty() || diag.primary_span.offset > source.bytes.size()) {
    fmt::format_to(std::back_inserter(out), ":{}\n", diag.primary_span.offset);
    return;
  }

  const LineInfo info = locate(source.bytes, diag.primary_span.offset);
  fmt::format_to(std::back_inserter(out), ":{}:{}\n", info.line, info.col);

  const u32 gutter = decimal_width(info.line);
  write_gutter(out, gutter);
  fmt::format_to(std::back_inserter(out), "\n");
  begin_style(out, options.color, term::kBlue);
  fmt::format_to(std::back_inserter(out), "{}", info.line);
  end_style(out, options.color);
  fmt::format_to(std::back_inserter(out), " | {}\n", info.text);

  // Caret run clipped to the rendered line (at least one caret).
  // locate() clamps out-of-range offsets; mirror that here.
  u32 line_offset = diag.primary_span.offset;
  if (line_offset > source.bytes.size()) {
    line_offset = static_cast<u32>(source.bytes.size());
  }
  const u32 line_start = line_offset - (info.col - 1);
  u32 line_end = line_start + static_cast<u32>(info.text.size());
  u32 caret_end = line_offset + diag.primary_span.length;
  if (caret_end > line_end) {
    caret_end = line_end;
  }
  u32 carets = caret_end > line_offset ? caret_end - line_offset : 0;
  if (carets == 0) {
    carets = 1;
  }
  write_gutter(out, gutter);
  out.push_back(' ');
  for (u32 i = 1; i < info.col; ++i) {
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
        label_source.name.empty() ? "[unknown file]" : label_source.name;
    fmt::format_to(std::back_inserter(out), " = ");
    begin_style(out, options.color, term::kCyan, true);
    fmt::format_to(std::back_inserter(out), "note");
    end_style(out, options.color);
    fmt::format_to(std::back_inserter(out), ": {} --> ", label.message);
    append_colored(out, options.color, term::kCyan, label_name);
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
