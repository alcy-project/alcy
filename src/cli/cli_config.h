// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string_view>
#include <vector>

#include "fpag/base/numeric.h"
#include "fpag/term/color_mode.h"

namespace cli {

enum class Subcommand : u8 {
  None,
  Build,
  Run,
  New,
  Init,
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
  // System linker driver for executable builds (empty selects the default
  // toolchain driver). Borrows argv storage like target_dir.
  std::string_view linker;
  // Trailing positionals after the target, passed to the program by
  // `run`. Views borrow argv storage like target_dir.
  std::vector<std::string_view> program_args;

  constexpr bool operator==(const CliConfig&) const = default;
};

}  // namespace cli
