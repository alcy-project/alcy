// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/lsp.h"

#include "config/build_config.h"
#include "fpag/base/numeric.h"

#if BUILD_FLAG(IS_OS_WIN)
#include <fcntl.h>
#include <io.h>
#endif

#include "lsp/server.h"
#include "lsp/transport.h"

namespace lsp {

i32 run(const RunOptions& options) {
#if BUILD_FLAG(IS_OS_WIN)
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
