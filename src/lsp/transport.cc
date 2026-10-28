// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/transport.h"

#include <cerrno>
#include <string>
#include <string_view>

#include "config/build_config.h"

#if BUILD_FLAG(IS_OS_WIN)
#include <io.h>
#else
#include <unistd.h>
#endif

#include "fpag/base/numeric.h"
#include "lsp/frame.h"

namespace lsp {

namespace {

constexpr usize K_CHUNK_SIZE = 8192;

isize read_once(i32 fd, void* buffer, usize size) {
#if BUILD_FLAG(IS_OS_WIN)
  return static_cast<isize>(_read(fd, buffer, static_cast<u32>(size)));
#else
  for (;;) {
    const isize count = ::read(fd, buffer, size);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    return static_cast<isize>(count);
  }
#endif
}

isize write_once(i32 fd, const void* buffer, usize size) {
#if BUILD_FLAG(IS_OS_WIN)
  return static_cast<isize>(_write(fd, buffer, static_cast<u32>(size)));
#else
  for (;;) {
    const isize count = ::write(fd, buffer, size);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    return static_cast<isize>(count);
  }
#endif
}

}  // namespace

bool Transport::read_more() {
  if (eof_) {
    return false;
  }
  if (start_ > 0) {
    buffer_.erase(0, start_);
    start_ = 0;
  }
  char chunk[K_CHUNK_SIZE];
  const isize count = read_once(input_fd_, chunk, sizeof(chunk));
  if (count <= 0) {
    eof_ = true;
    return false;
  }
  buffer_.append(chunk, static_cast<usize>(count));
  return true;
}

bool Transport::ensure(usize count) {
  while (available().size() < count) {
    if (!read_more()) {
      return false;
    }
  }
  return true;
}

bool Transport::read_message(std::string* out) {
  if (failed_) {
    return false;
  }
  FrameHeader header;
  for (;;) {
    header = scan_frame_header(available());
    if (header.status == FrameStatus::Malformed) {
      failed_ = true;
      return false;
    }
    if (header.status == FrameStatus::Complete) {
      break;
    }
    if (!read_more()) {
      failed_ = true;
      return false;
    }
  }
  if (!ensure(header.header_length + header.content_length)) {
    failed_ = true;
    return false;
  }
  const std::string_view view = available();
  out->assign(view.data() + header.header_length, header.content_length);
  start_ += header.header_length + header.content_length;
  return true;
}

bool Transport::write_message(std::string_view payload) {
  std::string frame;
  append_frame(frame, payload);
  usize written = 0;
  while (written < frame.size()) {
    const isize count =
        write_once(output_fd_, frame.data() + written, frame.size() - written);
    if (count <= 0) {
      failed_ = true;
      return false;
    }
    written += static_cast<usize>(count);
  }
  return true;
}

}  // namespace lsp
