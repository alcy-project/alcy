// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "tcp/listener.h"

#include <utility>

#include "tcp/connection.h"
#include "tcp/endpoint.h"
#include "tcp/error.h"
#include "tcp/tcp.h"

namespace tcp {

Listener::Listener(Listener&& other) noexcept
    : handle_(std::exchange(other.handle_, INVALID_HANDLE)) {}

Listener& Listener::operator=(Listener&& other) noexcept {
  if (this != &other) {
    close();
    handle_ = std::exchange(other.handle_, INVALID_HANDLE);
  }
  return *this;
}

Listener::~Listener() {
  close();
}

base::Result<Listener, Error> Listener::listen(const Endpoint& endpoint,
                                               u16* bound_port) {
  base::Result<Handle, Error> handle = tcp::open_listener(endpoint, bound_port);
  if (handle.is_err()) {
    return base::make_err(std::move(handle).unwrap_err());
  }
  return base::make_ok(Listener{std::move(handle).unwrap()});
}

base::Result<Connection, Error> Listener::accept() {
  if (!valid()) {
    return base::make_err(Error::AcceptFailed);
  }
  base::Result<Handle, Error> handle = tcp::accept(handle_);
  if (handle.is_err()) {
    return base::make_err(std::move(handle).unwrap_err());
  }
  return base::make_ok(Connection{std::move(handle).unwrap()});
}

void Listener::close() {
  if (valid()) {
    tcp::close(handle_);
    handle_ = INVALID_HANDLE;
  }
}

}  // namespace tcp
