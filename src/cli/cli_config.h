// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string_view>
#include <vector>

#include "fpag/base/numeric.h"
#include "fpag/term/color_mode.h"
#include "pipeline/emit_mode.h"
#include "pipeline/vcs.h"

namespace cli {

enum class Subcommand : u8 {
  None,
  Build,
  Compile,
  Run,
  New,
  Init,
  Check,
};

struct CliConfig {
  bool time_trace = false;
  // Presentation preference; cli resolves terminal capability before dispatch.
  term::ColorMode color_mode = term::ColorMode::Auto;
  Subcommand subcommand = Subcommand::None;
  bool release = false;
  // First positional, read as each subcommand's target (empty when
  // absent; the cli substitutes "."). Borrows argv storage, so a config
  // must not outlive the argument vector it was parsed from.
  std::string_view target_dir;
  // Single file for `check --file`, naming one source explicitly so the
  // positional stays a package directory. Borrows argv storage like
  // target_dir.
  std::string_view file;
  // Where the output goes (empty selects a path beside the input, or the
  // package's out/ directory). Borrows argv storage like target_dir.
  std::string_view output;
  // What the build writes. Defaults to an executable, so the name says
  // which one only when it is not the default.
  pipeline::EmitMode emit = pipeline::EmitMode::Executable;
  // Compile the program on standard input rather than a target. The name it
  // is reported under is <stdin>, since a pipe carries no file behind it.
  bool stdin_source = false;
  // System linker driver for executable builds (empty selects the default
  // toolchain driver). Borrows argv storage like target_dir.
  std::string_view linker;
  // Trailing positionals after the target, passed to the program by
  // `run`. Views borrow argv storage like target_dir.
  std::vector<std::string_view> program_args;
  // `compile` starts with no standard library: nothing is staged unless
  // `--deps` names it. Views borrow argv storage like target_dir.
  bool no_std = false;
  // `compile` dependencies on top of the default suite, in `--deps`
  // fragment form (`alcy/std/core`, or `name = { ... }`). Views borrow
  // argv storage like target_dir.
  std::vector<std::string_view> deps;
  // Which ignore file `new` and `init` write. Defaults to git because
  // that is the version control most packages are kept in, and the
  // choice only selects a file: no repository is created either way.
  pipeline::Vcs vcs = pipeline::Vcs::Git;

  constexpr bool operator==(const CliConfig&) const = default;
};

}  // namespace cli
