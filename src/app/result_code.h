// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "fpag/base/numeric.h"

namespace app {

enum class ResultCode : u8 {
  Success = 0,
  ArgParseError,
  NotImplemented,
  BuildFailed,
  CheckFailed,
  // TODO
  // LexError,
  // ParseError,
  // SemaError,
  // CodeGenError,
  // LinkError,
};

inline constexpr i32 result_code(ResultCode code) {
  return static_cast<i32>(code);
}

}  // namespace app
