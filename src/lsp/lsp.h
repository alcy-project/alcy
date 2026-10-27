// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "fpag/base/numeric.h"

namespace lsp {

// Where the server reads requests and writes responses. The defaults are
// the process's standard streams, which is what an editor launches.
struct RunOptions {
  i32 input_fd = 0;
  i32 output_fd = 1;
};

// Serves one client until it sends `exit` or closes its input, and
// returns the exit code the protocol asks for. Standard output carries
// nothing but protocol frames.
i32 run(const RunOptions& options);

}  // namespace lsp
