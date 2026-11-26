// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "tcp/connection.h"

#include <span>
#include <string_view>
#include <utility>

#include "tcp/endpoint.h"
#include "tcp/error.h"
#include "tcp/tcp.h"

namespace tcp {

Connection::Connection(Connection&& other) noexcept
    : handle_(std::exchange(other.handle_, INVALID_HANDLE)) {}

Connection& Connection::operator=(Connection&& other) noexcept {
  if (this != &other) {
    close();
    handle_ = std::exchange(other.handle_, INVALID_HANDLE);
  }
  return *this;
}

Connection::~Connection() {
  close();
}

base::Result<Connection, Error> Connection::connect(const Endpoint& endpoint) {
  base::Result<Handle, Error> handle = tcp::connect(endpoint);
  if (handle.is_err()) {
    return base::make_err(std::move(handle).unwrap_err());
  }
  return base::make_ok(Connection{std::move(handle).unwrap()});
}

base::Result<usize, Error> Connection::read(std::span<char> bytes) {
  if (!valid()) {
    return base::make_err(Error::Disconnected);
  }
  return tcp::read(handle_, bytes);
}

base::Result<void, Error> Connection::write(std::string_view bytes) {
  if (!valid()) {
    return base::make_err(Error::Disconnected);
  }
  return tcp::write(handle_, bytes);
}

void Connection::close() {
  if (valid()) {
    tcp::close(handle_);
    handle_ = INVALID_HANDLE;
  }
}

}  // namespace tcp
