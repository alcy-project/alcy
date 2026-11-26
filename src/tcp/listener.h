// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "tcp/connection.h"
#include "tcp/endpoint.h"
#include "tcp/error.h"
#include "tcp/tcp.h"

namespace tcp {

// A listening socket. One accept is enough for the editor server, but
// the type does not assume how many connections a caller will take.
class Listener {
 public:
  Listener() = default;
  explicit Listener(Handle handle) : handle_(handle) {}
  Listener(Listener&& other) noexcept;
  Listener& operator=(Listener&& other) noexcept;
  Listener(const Listener&) = delete;
  Listener& operator=(const Listener&) = delete;
  ~Listener();

  // Port zero asks the OS for a free port; `bound_port` then receives
  // the number it chose. Editor invocations pass an explicit port.
  static base::Result<Listener, Error> listen(const Endpoint& endpoint,
                                              u16* bound_port = nullptr);

  base::Result<Connection, Error> accept();

  void close();
  bool valid() const { return handle_ != INVALID_HANDLE; }

 private:
  Handle handle_ = INVALID_HANDLE;
};

}  // namespace tcp
