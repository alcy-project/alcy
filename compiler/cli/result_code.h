// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "fpag/base/numeric.h"

namespace cli {

enum class ResultCode : u8 {
  // Values are a stable cli contract: never renumber existing codes.
  // (2 was NotImplemented, retired with the `test` stub.)
  Success = 0,
  ArgParseError = 1,
  BuildFailed = 3,
  CheckFailed = 4,
  NewFailed = 5,
  RunFailed = 6,
};

inline constexpr i32 result_code(ResultCode code) {
  return static_cast<i32>(code);
}

}  // namespace cli
