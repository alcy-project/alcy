// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#include "app/driver_config.h"

namespace app {

ValidationStatus validate_config(const DriverConfig& config) {
  using Vs = ValidationStatus;

  if (config.color_mode == term::ColorMode::Unknown) [[unlikely]] {
    return Vs::InvalidColorMode;
  }

  return Vs::Success;
}

}  // namespace app
