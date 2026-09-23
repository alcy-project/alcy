// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string_view>

#include "fpag/base/numeric.h"
#include "fpag/term/color_mode.h"

namespace cli {

enum class Subcommand : u8 {
  None,
  Build,
  Test,
  Run,
  New,
  Check,
};

struct CliConfig {
  bool time_trace = false;
  term::ColorMode color_mode = term::ColorMode::Auto;
  Subcommand subcommand = Subcommand::None;
  bool release = false;
  // Positional target for build (empty when absent; the cli substitutes
  // "."). Borrows argv storage, so a config must not outlive the argument
  // vector it was parsed from.
  std::string_view target_dir;
  // Object output path for single-file builds (empty selects next to the
  // input with a .o suffix). Borrows argv storage like target_dir.
  std::string_view output;

  constexpr bool operator==(const CliConfig&) const = default;
};

}  // namespace cli
