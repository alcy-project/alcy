// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/lsp.h"

#include <cstdio>
#include <string_view>
#include <utility>

#include "config/build_config.h"
#include "fpag/base/numeric.h"
#include "tcp/connection.h"
#include "tcp/error.h"
#include "tcp/init.h"
#include "tcp/listener.h"

#if BUILD_FLAG(IS_OS_WIN)
#include <fcntl.h>
#include <io.h>
#endif

#include "lsp/server.h"
#include "lsp/transport.h"

namespace lsp {

namespace {

i32 report(const char* prefix, tcp::Error error) {
  const std::string_view text = tcp::message(error);
  std::fprintf(stderr, "alcy lsp: %s: %.*s\n", prefix,
               static_cast<i32>(text.size()), text.data());
  return 1;
}

}  // namespace

i32 run(const RunOptions& options) {
  if (options.transport == RunOptions::TransportKind::Stdio) {
#if BUILD_FLAG(IS_OS_WIN)
    // The frame's CRLF is protocol, not line ending: a console in text mode
    // would translate every one of them and desynchronize the peer.
    _setmode(options.input_fd, _O_BINARY);
    _setmode(options.output_fd, _O_BINARY);
#endif
    Transport transport(Channel{Channel::Kind::Fds, options.input_fd,
                                options.output_fd, nullptr});
    Server server(transport);
    return server.run();
  }

  // Winsock is process state, so the guard outlives every socket here.
  tcp::Init init;
  if (!init.ok()) {
    return report("network initialization failed", tcp::Error::InitFailed);
  }
  base::Result<tcp::Listener, tcp::Error> listener =
      tcp::Listener::listen(options.endpoint);
  if (listener.is_err()) {
    return report("could not listen", std::move(listener).unwrap_err());
  }
  base::Result<tcp::Connection, tcp::Error> connection =
      listener.value().accept();
  // The accepted session owns its port from here; stop listening before
  // serving it.
  listener.value().close();
  if (connection.is_err()) {
    return report("could not accept", std::move(connection).unwrap_err());
  }
  Transport transport(
      Channel{Channel::Kind::Socket, 0, 0, &connection.value()});
  Server server(transport);
  return server.run();
}

}  // namespace lsp
