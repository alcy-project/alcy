// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/frame.h"

#include <charconv>
#include <string>
#include <string_view>
#include <system_error>

#include "fpag/base/numeric.h"

namespace lsp {

namespace {

constexpr std::string_view HEADER_TERMINATOR = "\r\n\r\n";
constexpr std::string_view LINE_TERMINATOR = "\r\n";
constexpr std::string_view CONTENT_LENGTH = "content-length";

std::string_view trim(std::string_view text) {
  usize begin = 0;
  while (begin < text.size() && (text[begin] == ' ' || text[begin] == '\t')) {
    ++begin;
  }
  usize end = text.size();
  while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t')) {
    --end;
  }
  return text.substr(begin, end - begin);
}

bool equals_ignore_case(std::string_view left, std::string_view right) {
  if (left.size() != right.size()) {
    return false;
  }
  for (usize i = 0; i < left.size(); ++i) {
    char a = left[i];
    char b = right[i];
    if (a >= 'A' && a <= 'Z') {
      a = static_cast<char>(a - 'A' + 'a');
    }
    if (b >= 'A' && b <= 'Z') {
      b = static_cast<char>(b - 'A' + 'a');
    }
    if (a != b) {
      return false;
    }
  }
  return true;
}

bool parse_length(std::string_view digits, usize* out) {
  if (digits.empty()) {
    return false;
  }
  u64 value = 0;
  const char* begin = digits.data();
  const char* end = begin + digits.size();
  const std::from_chars_result result = std::from_chars(begin, end, value);
  if (result.ec != std::errc() || result.ptr != end) {
    return false;
  }
  if (value > static_cast<u64>(~static_cast<usize>(0))) {
    return false;
  }
  *out = static_cast<usize>(value);
  return true;
}

}  // namespace

FrameHeader scan_frame_header(std::string_view buffer) {
  const usize terminator = buffer.find(HEADER_TERMINATOR);
  if (terminator == std::string_view::npos) {
    return FrameHeader{};
  }
  const std::string_view header = buffer.substr(0, terminator);
  std::string_view content_length;
  usize line_start = 0;
  for (;;) {
    const usize line_end = header.find(LINE_TERMINATOR, line_start);
    const std::string_view line =
        line_end == std::string_view::npos
            ? header.substr(line_start)
            : header.substr(line_start, line_end - line_start);
    const usize colon = line.find(':');
    if (colon == std::string_view::npos) {
      return FrameHeader{FrameStatus::Malformed, 0, 0};
    }
    if (equals_ignore_case(trim(line.substr(0, colon)), CONTENT_LENGTH)) {
      content_length = trim(line.substr(colon + 1));
    }
    if (line_end == std::string_view::npos) {
      break;
    }
    line_start = line_end + LINE_TERMINATOR.size();
  }
  usize parsed = 0;
  if (!parse_length(content_length, &parsed)) {
    return FrameHeader{FrameStatus::Malformed, 0, 0};
  }
  return FrameHeader{FrameStatus::Complete,
                     terminator + HEADER_TERMINATOR.size(), parsed};
}

void append_frame(std::string& out, std::string_view payload) {
  out += "Content-Length: ";
  out += std::to_string(payload.size());
  out += "\r\n\r\n";
  out.append(payload.data(), payload.size());
}

}  // namespace lsp
