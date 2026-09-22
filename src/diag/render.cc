// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "diag/render.h"

#include <iterator>
#include <string_view>

#include "diag/diagnostic.h"
#include "diag/span.h"
#include "fmt/color.h"
#include "fmt/core.h"
#include "fmt/format.h"
#include "fpag/base/numeric.h"

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
    const fmt::color color =
        diag.severity == Severity::Error
            ? fmt::color::red
            : (diag.severity == Severity::Warning ? fmt::color::yellow
                                                  : fmt::color::cyan);
    fmt::format_to(std::back_inserter(out),
                   fmt::fg(color) | fmt::emphasis::bold, "{}", sev);
    fmt::format_to(std::back_inserter(out), "[{}{}]: {}\n",
                   severity_code(diag.severity), diag.code, diag.message);
  } else {
    fmt::format_to(std::back_inserter(out), "{}[{}{}]: {}\n", sev,
                   severity_code(diag.severity), diag.code, diag.message);
  }

  if (!diag.has_primary_span) {
    return;
  }

  SourceText source;
  if (fetch != nullptr) {
    source = fetch(diag.primary_span.file, ctx);
  }
  const std::string_view name =
      source.name.empty() ? "[unknown file]" : source.name;

  if (source.bytes.empty()) {
    fmt::format_to(std::back_inserter(out), " --> {}:{}\n", name,
                   diag.primary_span.offset);
    return;
  }

  const LineInfo info = locate(source.bytes, diag.primary_span.offset);
  fmt::format_to(std::back_inserter(out), " --> {}:{}:{}\n", name, info.line,
                 info.col);

  const u32 gutter = decimal_width(info.line);
  write_gutter(out, gutter);
  fmt::format_to(std::back_inserter(out), "\n");
  fmt::format_to(std::back_inserter(out), "{} | {}\n", info.line, info.text);

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
  for (u32 i = 0; i < carets; ++i) {
    out.push_back('^');
  }
  out.push_back('\n');

  for (u32 i = 0; i < diag.label_count; ++i) {
    const Label& label = diag.labels[i];
    SourceText label_source;
    if (fetch != nullptr) {
      label_source = fetch(label.span.file, ctx);
    }
    const std::string_view label_name =
        label_source.name.empty() ? "[unknown file]" : label_source.name;
    if (label_source.bytes.empty()) {
      fmt::format_to(std::back_inserter(out), " = note: {} --> {}:{}\n",
                     label.message, label_name, label.span.offset);
      continue;
    }
    const LineInfo label_info = locate(label_source.bytes, label.span.offset);
    fmt::format_to(std::back_inserter(out), " = note: {} --> {}:{}:{}\n",
                   label.message, label_name, label_info.line, label_info.col);
  }
}

}  // namespace diag
