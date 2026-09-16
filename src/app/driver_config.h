// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#pragma once

#include "fpag/base/numeric.h"
#include "fpag/term/color_mode.h"

namespace app {

struct DriverConfig {
  bool time_trace;
  term::ColorMode color_mode;
};

enum class ValidationStatus : u8 {
  Success,
  InvalidColorMode,
};

ValidationStatus validate_config(const DriverConfig& config);

}  // namespace app
