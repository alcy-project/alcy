// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "tcp/endpoint.h"

#include <string_view>

namespace tcp {

bool is_loopback(std::string_view host) {
  return host.empty() || host == "localhost" || host == "127.0.0.1" ||
         host == "::1";
}

}  // namespace tcp
