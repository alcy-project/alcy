// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "tcp/tcp.h"

namespace tcp {

// Keeps the platform's network library alive for the lifetime of one
// server run. On platforms that need no initialization it is a no-op.
class Init {
 public:
  Init() = default;
  Init(const Init&) = delete;
  Init& operator=(const Init&) = delete;

  bool ok() const { return platform_.ok(); }

 private:
  Platform platform_;
};

}  // namespace tcp
