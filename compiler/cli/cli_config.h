// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string_view>
#include <vector>

#include "codegen/backend.h"
#include "codegen/target.h"
#include "fpag/base/numeric.h"
#include "fpag/term/color_mode.h"
#include "i18n/language.h"
#include "pipeline/emit_mode.h"
#include "pipeline/link_options.h"
#include "pipeline/vcs.h"
#include "pkg/toolchain.h"

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
  // Language every message of this invocation is written in. Asked for
  // explicitly and defaulted here: a build must not read a language out
  // of the environment, or the same command line reports different text
  // on different machines.
  i18n::Language language = i18n::Language::EnUs;
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
  // Which backend writes the machine code. None means the command named
  // none, and the pipeline picks the first backend that can write for the
  // chosen target.
  codegen::Backend backend = codegen::Backend::None;
  // The machine to build for, from --target; the host when unnamed.
  codegen::Target target = codegen::host_target();
  // Compile the program on standard input rather than a target. The name it
  // is reported under is <stdin>, since a pipe carries no file behind it.
  bool stdin_source = false;
  // System linker driver for executable builds (empty selects the default
  // toolchain driver). Borrows argv storage like target_dir.
  std::string_view linker;
  // Arguments handed to that driver, in `--link-args` order: one flag per
  // argument, so nothing has to be split or quoted. Replaces the file's
  // own list when given. Views borrow argv storage like target_dir.
  std::vector<std::string_view> link_args;
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
  // `--suite` for `new` and `init`: `<name>` or `<owner>/<name>`
  // scaffolds a suite, and an empty value scaffolds a package (which
  // joins an enclosing suite when one is found). Borrows argv storage
  // like target_dir.
  std::string_view suite = "";
  // Emit the result as one JSON document on standard output instead of
  // the text report, for an editor or another tool reading it.
  bool json = false;
  // How many threads may read the source at once. Zero means the command
  // line did not say, which the pipeline answers with one.
  u32 jobs = 0;

  constexpr bool operator==(const CliConfig&) const = default;
};

// What the link step gets for this invocation: the goal package's
// toolchain file, overridden wherever the command named something. An
// absent file is an empty Toolchain, which is what a command that reads
// no file passes.
pipeline::LinkOptions resolve_link_options(const CliConfig& config,
                                           const pkg::Toolchain& tool);

}  // namespace cli
