// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "tcp/error.h"

#include <string_view>

namespace tcp {

std::string_view message(Error error) {
  switch (error) {
    case Error::InitFailed: return "network initialization failed";
    case Error::ResolveFailed: return "could not resolve the address";
    case Error::SocketFailed: return "could not create a socket";
    case Error::BindFailed: return "could not bind the address";
    case Error::ListenFailed: return "could not listen on the socket";
    case Error::AcceptFailed: return "could not accept a connection";
    case Error::ConnectFailed: return "could not connect to the address";
    case Error::ReadFailed: return "could not read from the connection";
    case Error::WriteFailed: return "could not write to the connection";
    case Error::Disconnected: return "the connection closed";
  }
  return "network error";
}

}  // namespace tcp
