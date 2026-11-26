// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string_view>

#include "fpag/base/numeric.h"

namespace tcp {

enum class Error : u8 {
  InitFailed,
  ResolveFailed,
  SocketFailed,
  BindFailed,
  ListenFailed,
  AcceptFailed,
  ConnectFailed,
  ReadFailed,
  WriteFailed,
  Disconnected,
};

std::string_view message(Error error);

}  // namespace tcp
