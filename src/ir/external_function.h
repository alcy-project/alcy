// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#pragma once

#include "fpag/base/numeric.h"
#include "fpag/str/string_pool_id.h"
#include "ir/function.h"

namespace ir {

enum class CallingConvention : u8 {
  C,

  // Fast,

  // StdCall,  // Win32 API (x86)
  // Win64,    // Windows x64
  // SysV,     // Linux/macOS x64

  // GHC,        // Haskell
  // WebKit_JS,  // JavaScriptCore
};

struct ExternalFunction {
  FunctionMeta meta;

  CallingConvention calling_conv;
};

}  // namespace ir
