// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <span>
#include <string_view>

#include "diag/bag.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/mem/arena.h"
#include "source/source.h"

namespace pkg {

// Directory beside alcy.toml holding the goal package's own build
// configuration. A package built as someone else's dependency never
// has this directory read; see docs/adr/0019.
constexpr std::string_view CONFIG_DIR_NAME = ".alcy";

// Toolchain selection for the goal package. Optional file; absent
// means defaults everywhere.
constexpr std::string_view TOOLCHAIN_FILE_NAME = "toolchain.toml";

// How the goal package links: the system driver handed to the link
// step, and the arguments that driver is given. An empty driver
// selects the default toolchain driver. Views borrow arena storage
// owned by the caller of parse_toolchain().
struct Toolchain {
  std::string_view linker;
  std::span<const std::string_view> link_args;
};

// Parses `.alcy/toolchain.toml` bytes. Unknown keys are ignored, like
// alcy.toml; a mistyped known key is an error.
base::Result<Toolchain, diag::Reported> parse_toolchain(
    std::string_view bytes,
    std::string_view filename,
    source::FileId /*file*/,
    diag::DiagBag& bag,
    mem::Arena& arena);

}  // namespace pkg
