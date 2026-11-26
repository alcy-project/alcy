// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "fpag/base/numeric.h"
#include "tcp/endpoint.h"

namespace lsp {

// Where the server reads requests and writes responses. The default is
// the process's standard streams, which is what an editor launches.
// A TCP run accepts one connection and exits when that session ends.
struct RunOptions {
  enum class TransportKind : u8 { Stdio, Tcp };

  TransportKind transport = TransportKind::Stdio;
  i32 input_fd = 0;
  i32 output_fd = 1;
  tcp::Endpoint endpoint;
};

// Serves one client until it sends `exit` or closes its input, and
// returns the exit code the protocol asks for. Standard output carries
// nothing but protocol frames in either transport mode.
i32 run(const RunOptions& options);

}  // namespace lsp
