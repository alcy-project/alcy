// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "fpag/base/numeric.h"

namespace cli {

enum class ResultCode : u8 {
  Success = 0,
  ArgParseError,
  NotImplemented,
  BuildFailed,
  CheckFailed,
  NewFailed,
};

inline constexpr i32 result_code(ResultCode code) {
  return static_cast<i32>(code);
}

}  // namespace cli
