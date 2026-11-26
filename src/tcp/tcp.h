// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <span>
#include <string_view>

#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "tcp/endpoint.h"
#include "tcp/error.h"

namespace tcp {

// A socket descriptor. Invalid is negative on both platforms: POSIX
// uses -1, and Windows stores SOCKET in the same signed slot.
using Handle = i64;
constexpr Handle INVALID_HANDLE = -1;

// Starts and stops the platform's network library. The guard is
// process-wide because Winsock initialization is process-wide.
class Platform {
 public:
  Platform();
  ~Platform();
  Platform(const Platform&) = delete;
  Platform& operator=(const Platform&) = delete;

  bool ok() const { return ok_; }

 private:
  bool ok_ = true;
};

base::Result<Handle, Error> open_listener(const Endpoint& endpoint,
                                          u16* bound_port);
base::Result<Handle, Error> accept(Handle listener);
base::Result<Handle, Error> connect(const Endpoint& endpoint);
base::Result<usize, Error> read(Handle handle, std::span<char> bytes);
base::Result<void, Error> write(Handle handle, std::string_view bytes);
void close(Handle handle);

}  // namespace tcp
