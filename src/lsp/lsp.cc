// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/lsp.h"

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

#include "lsp/server.h"
#include "lsp/transport.h"

namespace lsp {

i32 run(const RunOptions& options) {
#if defined(_WIN32)
  // The frame's CRLF is protocol, not line ending: a console in text mode
  // would translate every one of them and desynchronize the peer.
  _setmode(options.input_fd, _O_BINARY);
  _setmode(options.output_fd, _O_BINARY);
#endif
  Transport transport(options.input_fd, options.output_fd);
  Server server(transport);
  return server.run();
}

}  // namespace lsp
