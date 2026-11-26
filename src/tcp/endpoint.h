// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string>
#include <string_view>

#include "fpag/base/numeric.h"

namespace tcp {

// A host and port a listener binds or a client connects to. An empty
// host means the local loopback address.
struct Endpoint {
  std::string host = "127.0.0.1";
  u16 port = 0;
};

// Whether a host names the local machine. Binding any other address
// would expose a server that has no authentication.
bool is_loopback(std::string_view host);

}  // namespace tcp
