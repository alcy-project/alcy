// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/transport.h"

#include <cerrno>
#include <span>
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
#include "tcp/error.h"

namespace lsp {

namespace {

constexpr usize CHUNK_SIZE = 8192;

isize read_once(const Channel& channel, void* buffer, usize size) {
  if (channel.kind == Channel::Kind::Socket) {
    if (channel.socket == nullptr || !channel.socket->valid()) {
      return 0;
    }
    const base::Result<usize, tcp::Error> count =
        channel.socket->read(std::span<char>(static_cast<char*>(buffer), size));
    if (count.is_err()) {
      return -1;
    }
    return static_cast<isize>(count.value());
  }
#if BUILD_FLAG(IS_OS_WIN)
  return static_cast<isize>(
      _read(channel.input_fd, buffer, static_cast<u32>(size)));
#else
  for (;;) {
    const isize count = ::read(channel.input_fd, buffer, size);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    return static_cast<isize>(count);
  }
#endif
}

isize write_once(const Channel& channel, const void* buffer, usize size) {
  if (channel.kind == Channel::Kind::Socket) {
    if (channel.socket == nullptr || !channel.socket->valid()) {
      return -1;
    }
    const base::Result<void, tcp::Error> written = channel.socket->write(
        std::string_view(static_cast<const char*>(buffer), size));
    return written.is_ok() ? static_cast<isize>(size) : -1;
  }
#if BUILD_FLAG(IS_OS_WIN)
  return static_cast<isize>(
      _write(channel.output_fd, buffer, static_cast<u32>(size)));
#else
  for (;;) {
    const isize count = ::write(channel.output_fd, buffer, size);
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
  char chunk[CHUNK_SIZE];
  const isize count = read_once(channel_, chunk, sizeof(chunk));
  if (count < 0) {
    eof_ = true;
    failed_ = true;
    return false;
  }
  if (count == 0) {
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
  if (failed_ || eof_) {
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
      // A clean end of input is not a failed transport; a partial frame
      // left at that point is.
      if (eof_ && !failed_ && !available().empty()) {
        failed_ = true;
      }
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
        write_once(channel_, frame.data() + written, frame.size() - written);
    if (count <= 0) {
      failed_ = true;
      return false;
    }
    written += static_cast<usize>(count);
  }
  return true;
}

}  // namespace lsp
