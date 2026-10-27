// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/frame.h"

#include <string>

#include "doctest/doctest.h"

namespace lsp {

TEST_CASE("A complete frame header reports where its payload starts") {
  const FrameHeader header =
      scan_frame_header("Content-Length: 5\r\n\r\nhello");
  CHECK(header.status == FrameStatus::Complete);
  CHECK(header.header_length == 21);
  CHECK(header.content_length == 5);
}

TEST_CASE("A frame header name is case-insensitive and its value trimmed") {
  const FrameHeader header =
      scan_frame_header("content-length:  7\r\n\r\npayload");
  CHECK(header.status == FrameStatus::Complete);
  CHECK(header.content_length == 7);
}

TEST_CASE("A header without its terminator is incomplete") {
  const FrameHeader header = scan_frame_header("Content-Length: 5\r\n");
  CHECK(header.status == FrameStatus::Incomplete);
}

TEST_CASE("A header without a usable length is malformed") {
  CHECK(scan_frame_header("Content-Type: x\r\n\r\n").status ==
        FrameStatus::Malformed);
  CHECK(scan_frame_header("Content-Length: x\r\n\r\n").status ==
        FrameStatus::Malformed);
  CHECK(scan_frame_header("garbage\r\n\r\n").status == FrameStatus::Malformed);
}

TEST_CASE("A written frame carries the length it wrote") {
  std::string out;
  append_frame(out, "hi");
  CHECK(out == "Content-Length: 2\r\n\r\nhi");
}

}  // namespace lsp
