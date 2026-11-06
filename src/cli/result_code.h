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
  // A file the formatter could not read or write.
  FmtFailed = 7,
  // fmt --check found a file whose layout would change.
  FmtNotClean = 8,
  // A denied lint finding, or a lint run that could not read its
  // input.
  LintFailed = 9,
  // A test run that built its package and then saw a test fail. It shares
  // 1 with a rejected command line, the way a shell reports any command
  // that failed: the run's own status is zero and anything else is not.
  TestFailed = 1,
};

inline constexpr i32 result_code(ResultCode code) {
  return static_cast<i32>(code);
}

}  // namespace cli
