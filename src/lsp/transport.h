// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string>
#include <string_view>

#include "fpag/base/numeric.h"
#include "tcp/connection.h"

namespace lsp {

// Where a framed message comes from. A socket is one bidirectional
// stream; the standard streams are two descriptors. The channel does
// not own the socket: the server run that created it keeps it alive.
struct Channel {
  enum class Kind : u8 { Fds, Socket };

  Kind kind = Kind::Fds;
  i32 input_fd = 0;
  i32 output_fd = 1;
  tcp::Connection* socket = nullptr;
};

// Reads framed messages from one channel and writes them back. LSP puts
// a JSON-RPC message in each frame; the transport does not look at the
// payload.
//
// Reads are buffered, so a caller may hand the whole input at once.
// End of input is distinct from a read failure: the first ends a normal
// session, the second fails the transport.
class Transport {
 public:
  explicit Transport(Channel channel) : channel_(channel) {}

  // Reads the next message payload into `out`. False at end of input or
  // on a malformed frame.
  bool read_message(std::string* out);

  // Writes one message with its frame. False if the channel stopped
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

  Channel channel_;
  std::string buffer_;
  usize start_ = 0;
  bool eof_ = false;
  bool failed_ = false;
};

}  // namespace lsp
