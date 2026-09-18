// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#pragma once

#include <string_view>

#include "fpag/base/numeric.h"
#include "fpag/term/color_mode.h"

namespace app {

enum class Subcommand : u8 {
  None,
  Build,
  Test,
  Run,
  New,
  Check,
};

struct DriverConfig {
  bool time_trace;
  term::ColorMode color_mode;
  Subcommand subcommand = Subcommand::None;
  bool release = false;
  // Positional target for build (empty when absent; the driver substitutes
  // "."). Borrows argv storage.
  std::string_view target_dir;
};

constexpr bool operator==(const DriverConfig& lhs, const DriverConfig& rhs) {
  return lhs.time_trace == rhs.time_trace && lhs.color_mode == rhs.color_mode &&
         lhs.subcommand == rhs.subcommand && lhs.release == rhs.release &&
         lhs.target_dir == rhs.target_dir;
}

enum class ValidationStatus : u8 {
  Success,
  InvalidColorMode,
};

ValidationStatus validate_config(const DriverConfig& config);

}  // namespace app
