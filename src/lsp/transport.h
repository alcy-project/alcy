// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string>
#include <string_view>

#include "fpag/base/numeric.h"

namespace lsp {

// Reads framed messages from one file descriptor and writes them to
// another. LSP puts a JSON-RPC message in each frame; the transport does
// not look at the payload.
//
// Reads are buffered, so a caller may hand the whole input at once. Once
// a read fails, every later read fails too: a peer that closed or sent a
// malformed frame has no message to deliver after it.
class Transport {
 public:
  Transport(i32 input_fd, i32 output_fd)
      : input_fd_(input_fd), output_fd_(output_fd) {}

  // Reads the next message payload into `out`. False at end of input or
  // on a malformed frame.
  bool read_message(std::string* out);

  // Writes one message with its frame. False if the descriptor stopped
  // taking bytes.
  bool write_message(std::string_view payload);

  bool failed() const { return failed_; }
  bool eof() const { return eof_; }

 private:
  std::string_view available() const {
    return std::string_view(buffer_).substr(start_);
  }
  bool read_more();
  bool ensure(usize count);

  i32 input_fd_;
  i32 output_fd_;
  std::string buffer_;
  usize start_ = 0;
  bool eof_ = false;
  bool failed_ = false;
};

}  // namespace lsp
