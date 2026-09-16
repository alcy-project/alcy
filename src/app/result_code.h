// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#pragma once

#include "fpag/base/numeric.h"

namespace app {

enum class ResultCode : u8 {
  Success = 0,
  ArgParseError,
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
