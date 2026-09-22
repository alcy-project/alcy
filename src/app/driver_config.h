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
  bool time_trace = false;
  term::ColorMode color_mode = term::ColorMode::Auto;
  Subcommand subcommand = Subcommand::None;
  bool release = false;
  // Positional target for build (empty when absent; the driver substitutes
  // "."). Borrows argv storage, so a config must not outlive the argument
  // vector it was parsed from.
  std::string_view target_dir;
  // Object output path for single-file builds (empty selects next to the
  // input with a .o suffix). Borrows argv storage like target_dir.
  std::string_view output;

  constexpr bool operator==(const DriverConfig&) const = default;
};

}  // namespace app
