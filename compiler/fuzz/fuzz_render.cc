// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Adversarial spans through the diagnostic renderer.
//
// The renderer takes a byte offset and a length into a source buffer and
// turns them into a `line:col` and a caret run. Every field is
// attacker-influenced: a diagnostic comes from wherever an error was
// detected, and the span arithmetic is where the column bug lived, where
// byte columns put a caret after every multi-byte character.
//
// The property is self-consistency of the output: the column the header
// reports must be the column the caret line actually starts at, and both
// must stay inside the line. That is checkable by parsing the renderer's
// own output, so it needs no knowledge of the correct answer.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "diag/render.h"
#include "diag/span.h"
#include "diag/stage.h"
#include "fmt/format.h"
#include "fpag/base/numeric.h"
#include "fpag/mem/arena.h"
#include "i18n/language.h"

namespace {

// The buffer every span is measured against, and the fetch the renderer
// is handed. Fixed rather than derived from the input so a failure names
// one span shape instead of an input shape.
constexpr std::string_view SOURCE =
    "fn main() {\n"
    "  s := \"\xe6\x97\xa5\xe6\x9c\xac\" + tab;\n"
    "  x := @\n"
    "}\n"
    "\tindented\n";

std::optional<diag::SourceText> fetch(u32 file, const void*) {
  if (file == 1) {
    return diag::SourceText{"main.al", SOURCE};
  }
  return std::nullopt;
}

// The column the caret line starts at, counted after the "N | " gutter.
// Returns npos when the line does not look like a caret line.
usize caret_column(std::string_view caret_line) {
  const usize bar = caret_line.find('|');
  if (bar == std::string_view::npos) {
    return std::string_view::npos;
  }
  usize i = bar + 1;
  while (i < caret_line.size() && caret_line[i] == ' ') {
    ++i;
  }
  if (i < caret_line.size() && caret_line[i] == '^') {
    return i - (bar + 1);
  }
  return std::string_view::npos;
}

// The column the " --> file:line:col" header claims, after the last ':'.
usize header_column(std::string_view line) {
  const usize arrow = line.find("-->");
  if (arrow == std::string_view::npos) {
    return std::string_view::npos;
  }
  const std::string_view rest = line.substr(arrow + 3);
  const usize colon = rest.rfind(':');
  if (colon == std::string_view::npos) {
    return std::string_view::npos;
  }
  u32 value = 0;
  bool any = false;
  for (usize i = colon + 1; i < rest.size(); ++i) {
    if (rest[i] < '0' || rest[i] > '9') {
      return std::string_view::npos;
    }
    value = value * 10 + static_cast<u32>(rest[i] - '0');
    any = true;
  }
  return any ? value - 1 : std::string_view::npos;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, usize size) {
  if (size < 4) {
    return 0;
  }
  // The last four bytes pick the span, so the fuzzer explores the
  // arithmetic rather than the buffer.
  const u32 offset = (static_cast<u32>(data[size - 4]) << 24) |
                     (static_cast<u32>(data[size - 4 + 1]) << 16) |
                     (static_cast<u32>(data[size - 4 + 2]) << 8) |
                     static_cast<u32>(data[size - 4 + 3]);
  const u32 length = static_cast<u32>(data[size - 2]) * 64u;

  mem::Arena arena;
  arena.reserve(1u << 20);
  diag::DiagBag bag{arena, i18n::Language::EnUs};
  const u32 index = bag.emit_untranslated(
      diag::Severity::Error, diag::Stage::Lexer, 1,
      diag::Span{.file = 1, .offset = offset, .length = length}, "fuzz");
  const diag::Diagnostic* const diag = bag.at(index);
  if (diag == nullptr) {
    return 0;
  }

  fmt::memory_buffer out;
  diag::render(*diag, out, {}, fetch, nullptr);
  const std::string_view text(out.data(), out.size());

  // Out of range is a documented answer, not a failure: the renderer
  // prints the raw offset and stops.
  if (offset > SOURCE.size()) {
    return 0;
  }

  // Find the header and the caret line, and require they agree.
  usize header = std::string_view::npos;
  usize caret = std::string_view::npos;
  usize at = 0;
  while (at < text.size()) {
    const usize eol = text.find('\n', at);
    const std::string_view line = text.substr(
        at, eol == std::string_view::npos ? text.size() - at : eol - at);
    if (header == std::string_view::npos &&
        header_column(line) != std::string_view::npos) {
      header = header_column(line);
    }
    if (caret == std::string_view::npos &&
        caret_column(line) != std::string_view::npos) {
      caret = caret_column(line);
    }
    if (eol == std::string_view::npos) {
      break;
    }
    at = eol + 1;
  }
  if (header == std::string_view::npos || caret == std::string_view::npos) {
    return 0;
  }
  // One column apart: the header is 1-based, the caret offset is 0-based.
  if (header + 1 != caret) {
    // A disagreement is a rendering defect, and libFuzzer turns the
    // abort into a minimizing reproducer.
    __builtin_trap();
  }
  return 0;
}
