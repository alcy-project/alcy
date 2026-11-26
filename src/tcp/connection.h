// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <span>
#include <string_view>

#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "tcp/endpoint.h"
#include "tcp/error.h"
#include "tcp/tcp.h"

namespace tcp {

// One blocking, bidirectional stream. The handle is owned here and
// closed on destruction; copies would double-close it.
class Connection {
 public:
  Connection() = default;
  explicit Connection(Handle handle) : handle_(handle) {}
  Connection(Connection&& other) noexcept;
  Connection& operator=(Connection&& other) noexcept;
  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;
  ~Connection();

  static base::Result<Connection, Error> connect(const Endpoint& endpoint);

  base::Result<usize, Error> read(std::span<char> bytes);
  base::Result<void, Error> write(std::string_view bytes);
  void close();

  bool valid() const { return handle_ != INVALID_HANDLE; }
  Handle handle() const { return handle_; }

 private:
  Handle handle_ = INVALID_HANDLE;
};

}  // namespace tcp
