// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string>
#include <string_view>

#include "fpag/base/numeric.h"

namespace lsp {

// Whether a buffer holds a whole frame header yet.
enum class FrameStatus : u8 {
  Complete,
  Incomplete,
  Malformed,
};

// The header of one LSP base-protocol frame. `header_length` counts the
// bytes before the payload, terminator included.
struct FrameHeader {
  FrameStatus status = FrameStatus::Incomplete;
  usize header_length = 0;
  usize content_length = 0;
};

// Scans `buffer`, which must begin at a message boundary, for one
// `Content-Length` header. Incomplete means the terminator has not
// arrived; Malformed means it has and the header is not usable.
FrameHeader scan_frame_header(std::string_view buffer);

// Appends `payload` to `out` as one frame.
void append_frame(std::string& out, std::string_view payload);

}  // namespace lsp
